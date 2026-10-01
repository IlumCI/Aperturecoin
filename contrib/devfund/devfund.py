#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Development fund: 2-of-3 Taproot script-path multisig (doc/devfund-key-ceremony.md).

The fund output is

    tr(H, sortedmulti_a(2, A, B, C))

H is the BIP341 provably unspendable internal key, so the fund can only be
spent through its single leaf:

    <K1> OP_CHECKSIG <K2> OP_CHECKSIGADD <K3> OP_CHECKSIGADD OP_2 OP_NUMEQUAL

with K1 < K2 < K3 the holders' x-only public keys in byte order.

Commands (a spend is a JSON session file that holders pass around, like a PSBT):

    devfund.py script KEY KEY KEY [--threshold=2] [--hrp=sci]
        Script, address, descriptor and the chainparams line.
    devfund.py create KEY KEY KEY --utxo=TXID:VOUT:AMOUNT ... --pay=SCRIPTHEX:AMOUNT ... [--fee=SAT] -o session.json
        Unsigned spend. Amounts are in satoshis.
    devfund.py sign session.json --privkey=HEX
        Adds this holder's BIP340 signatures (one per input).
    devfund.py finalize session.json
        Prints the final transaction hex once the threshold is met.

The Python secp256k1 used here is not constant-time. Sign on an offline
machine, or with a signer that supports Taproot script paths.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "test", "functional"))

from test_framework.key import compute_xonly_pubkey, sign_schnorr  # noqa: E402
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, FromHex, ToHex  # noqa: E402
from test_framework.script import (  # noqa: E402
    CScript, LEAF_VERSION_TAPSCRIPT, OP_CHECKSIG, OP_CHECKSIGADD, OP_NUMEQUAL, SIGHASH_DEFAULT,
    TaprootSignatureHash, taproot_construct,
)
from test_framework.segwit_addr import encode_segwit_address  # noqa: E402

# BIP341 "H": a point with no known discrete logarithm.
NUMS_H = bytes.fromhex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")


def parse_keys(keys):
    out = sorted({bytes.fromhex(k) for k in keys})
    if len(out) != len(keys) or any(len(k) != 32 for k in out):
        raise SystemExit("keys must be distinct 32-byte x-only public keys (hex)")
    return out


def leaf_script(keys, threshold):
    ops = [keys[0], OP_CHECKSIG]
    for k in keys[1:]:
        ops += [k, OP_CHECKSIGADD]
    return CScript(ops + [threshold, OP_NUMEQUAL])


def fund(keys, threshold):
    leaf = leaf_script(keys, threshold)
    info = taproot_construct(NUMS_H, [("multi", leaf)])
    return leaf, info


def control_block(info):
    leaf = info.leaves["multi"]
    return bytes([leaf.version + info.negflag]) + info.inner_pubkey + leaf.merklebranch


def cmd_script(args):
    keys = parse_keys(args.keys)
    leaf, info = fund(keys, args.threshold)
    spk = bytes(info.scriptPubKey)
    print(json.dumps({
        "descriptor": f"tr({NUMS_H.hex()},sortedmulti_a({args.threshold},{','.join(k.hex() for k in keys)}))",
        "keys_sorted": [k.hex() for k in keys],
        "leaf_script": leaf.hex(),
        "script_pubkey": spk.hex(),
        "address": encode_segwit_address(args.hrp, 1, spk[2:]),
        "chainparams": f'consensus.devFundScript = ParseHex("{spk.hex()}");',
    }, indent=2))


def cmd_create(args):
    keys = parse_keys(args.keys)
    _, info = fund(keys, args.threshold)
    tx = CTransaction()
    tx.nVersion = 2
    spent = []
    for u in args.utxo:
        txid, vout, amount = u.split(":")
        tx.vin.append(CTxIn(COutPoint(int(txid, 16), int(vout)), nSequence=0xfffffffd))
        spent.append(int(amount))
    for p in args.pay:
        script, amount = p.split(":")
        tx.vout.append(CTxOut(int(amount), bytes.fromhex(script)))
    if sum(spent) - sum(o.nValue for o in tx.vout) != args.fee:
        raise SystemExit(f"inputs {sum(spent)} - outputs {sum(o.nValue for o in tx.vout)} != fee {args.fee}")
    session = {"keys": [k.hex() for k in keys], "threshold": args.threshold, "tx": ToHex(tx),
               "spent": [{"amount": a, "script": bytes(info.scriptPubKey).hex()} for a in spent], "sigs": {}}
    with open(args.output, "w", encoding="utf8") as f:
        json.dump(session, f, indent=2)
    print(f"wrote {args.output}")


def load(path):
    with open(path, encoding="utf8") as f:
        s = json.load(f)
    keys = [bytes.fromhex(k) for k in s["keys"]]
    leaf, info = fund(keys, s["threshold"])
    tx = FromHex(CTransaction(), s["tx"])
    spent = [CTxOut(e["amount"], bytes.fromhex(e["script"])) for e in s["spent"]]
    return s, keys, leaf, info, tx, spent


def sighashes(tx, spent, leaf):
    return [TaprootSignatureHash(tx, spent, SIGHASH_DEFAULT, i, scriptpath=True, script=leaf, leaf_ver=LEAF_VERSION_TAPSCRIPT)
            for i in range(len(tx.vin))]


def cmd_sign(args):
    s, keys, leaf, info, tx, spent = load(args.session)
    priv = bytes.fromhex(args.privkey)
    xonly = compute_xonly_pubkey(priv)[0]
    if xonly not in keys:
        raise SystemExit("this key is not one of the fund's keys")
    s["sigs"][xonly.hex()] = [sign_schnorr(priv, m).hex() for m in sighashes(tx, spent, leaf)]
    with open(args.session, "w", encoding="utf8") as f:
        json.dump(s, f, indent=2)
    print(f"signed by {xonly.hex()} ({len(s['sigs'])} of {s['threshold']})")


def cmd_finalize(args):
    s, keys, leaf, info, tx, spent = load(args.session)
    if len(s["sigs"]) < s["threshold"]:
        raise SystemExit(f"{len(s['sigs'])} signatures, {s['threshold']} needed")
    # Use exactly `threshold` signatures; the others are empty (counted as 0 by OP_CHECKSIGADD).
    signers = [k for k in keys if k.hex() in s["sigs"]][:s["threshold"]]
    cb = control_block(info)
    tx.wit.vtxinwit = []
    for i in range(len(tx.vin)):
        w = CTxInWitness()
        # Stack items are consumed by the keys from the last one to the first.
        w.scriptWitness.stack = [bytes.fromhex(s["sigs"][k.hex()][i]) if k in signers else b"" for k in reversed(keys)] + [leaf, cb]
        tx.wit.vtxinwit.append(w)
    print(ToHex(tx))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("script")
    p.add_argument("keys", nargs="+")
    p.add_argument("--threshold", type=int, default=2)
    p.add_argument("--hrp", default="sci")
    p = sub.add_parser("create")
    p.add_argument("keys", nargs="+")
    p.add_argument("--threshold", type=int, default=2)
    p.add_argument("--utxo", action="append", required=True)
    p.add_argument("--pay", action="append", required=True)
    p.add_argument("--fee", type=int, required=True)
    p.add_argument("-o", "--output", required=True)
    p = sub.add_parser("sign")
    p.add_argument("session")
    p.add_argument("--privkey", required=True)
    p = sub.add_parser("finalize")
    p.add_argument("session")
    args = ap.parse_args()
    if args.cmd in ("script", "create") and not 1 <= args.threshold <= len(args.keys) <= 16:
        raise SystemExit("threshold must be between 1 and the number of keys (at most 16)")
    {"script": cmd_script, "create": cmd_create, "sign": cmd_sign, "finalize": cmd_finalize}[args.cmd](args)


if __name__ == "__main__":
    main()
