#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tapscript covenants with the ApertureCoin opcode extensions (doc/covenants.md).

Spends P2TR script paths (no signatures) that use:
- OP_CHECKTEMPLATEVERIFY: only the committed spending template is valid
- OP_UTXOBYTECODE / OP_OUTPUTBYTECODE / values: a recursive pay-to-self vault
- OP_CAT: a hash-of-concatenation lock
- token introspection: at most 100 tokens may leave per spend
"""

import struct
from decimal import Decimal
from hashlib import sha256

from test_framework.key import compute_xonly_pubkey, generate_privkey
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, FromHex, ToHex
from test_framework.script import (
    CScript,
    LEAF_VERSION_TAPSCRIPT,
    OP_CAT,
    OP_CHECKTEMPLATEVERIFY,
    OP_EQUAL,
    OP_EQUALVERIFY,
    OP_GREATERTHANOREQUAL,
    OP_INPUTINDEX,
    OP_OUTPUTBYTECODE,
    OP_OUTPUTTOKENAMOUNT,
    OP_OUTPUTTOKENCATEGORY,
    OP_OUTPUTVALUE,
    OP_SHA256,
    OP_SUB,
    OP_SWAP,
    OP_TRUE,
    OP_UTXOBYTECODE,
    OP_UTXOTOKENAMOUNT,
    OP_UTXOTOKENCATEGORY,
    OP_UTXOVALUE,
    taproot_construct,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.tokens import encode_token_prefix
from test_framework.util import assert_equal

FEE = 20000


def ctv_hash(tx, input_index):
    """BIP119 standard template hash."""
    r = struct.pack("<i", tx.nVersion) + struct.pack("<I", tx.nLockTime)
    if any(len(i.scriptSig) for i in tx.vin):
        r += sha256(b"".join(bytes([len(i.scriptSig)]) + bytes(i.scriptSig) for i in tx.vin)).digest()
    r += struct.pack("<I", len(tx.vin))
    r += sha256(b"".join(struct.pack("<I", i.nSequence) for i in tx.vin)).digest()
    r += struct.pack("<I", len(tx.vout))
    r += sha256(b"".join(o.serialize() for o in tx.vout)).digest()
    r += struct.pack("<I", input_index)
    return sha256(r).digest()


class CovenantTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def taproot(self, script):
        info = taproot_construct(self.internal_key, [("leaf", script)])
        leaf = info.leaves["leaf"]
        control = bytes([leaf.version + info.negflag]) + info.inner_pubkey + leaf.merklebranch
        return info.scriptPubKey, control

    def fund(self, spk, value_sat, token_prefix=b"", extra_input=None):
        """Create an output with scriptPubKey token_prefix||spk; returns (txid, vout, value)."""
        fee_utxo = next(u for u in self.node.listunspent() if u["amount"] > 1)
        vin = [{"txid": fee_utxo["txid"], "vout": fee_utxo["vout"]}]
        total = int(fee_utxo["amount"] * 10**8)
        if extra_input:
            vin.append({"txid": extra_input[0], "vout": extra_input[1]})
            total += extra_input[2]
        change = Decimal(total - value_sat - 100000) / 10**8
        tx = FromHex(CTransaction(), self.node.createrawtransaction(vin, [{self.node.getnewaddress(): change}]))
        tx.vout.append(CTxOut(value_sat, CScript(token_prefix + bytes(spk))))
        signed = self.node.signrawtransactionwithwallet(ToHex(tx))
        assert signed["complete"]
        txid = self.node.sendrawtransaction(signed["hex"])
        self.node.generatetoaddress(1, self.addr)
        return txid, len(tx.vout) - 1, value_sat

    def spend(self, outpoint, script, control, outputs, stack=(), sequence=0xffffffff):
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(outpoint[0], 16), outpoint[1]), b"", sequence)]
        tx.vout = outputs
        tx.wit.vtxinwit = [CTxInWitness()]
        tx.wit.vtxinwit[0].scriptWitness.stack = list(stack) + [bytes(script), control]
        return tx

    def accept(self, tx):
        result = self.node.testmempoolaccept([ToHex(tx)])[0]
        assert result["allowed"], result
        self.node.sendrawtransaction(ToHex(tx))
        self.node.generatetoaddress(1, self.addr)

    def reject(self, tx, reason):
        result = self.node.testmempoolaccept([ToHex(tx)])[0]
        assert not result["allowed"], result
        assert reason in result["reject-reason"], result

    def run_test(self):
        self.node = self.nodes[0]
        self.addr = self.node.getnewaddress()
        self.node.generatetoaddress(110, self.addr)
        self.internal_key, _ = compute_xonly_pubkey(generate_privkey())
        dest = CScript(bytes.fromhex(self.node.getaddressinfo(self.node.getnewaddress())["scriptPubKey"]))

        self.log.info("OP_CHECKTEMPLATEVERIFY: only the committed template can spend")
        template = CTransaction()
        template.nVersion = 2
        template.vin = [CTxIn(COutPoint(0, 0), b"", 0xffffffff)]
        template.vout = [CTxOut(1_000_000 - FEE, dest)]
        ctv_script = CScript([ctv_hash(template, 0), OP_CHECKTEMPLATEVERIFY])
        spk, control = self.taproot(ctv_script)
        outpoint = self.fund(spk, 1_000_000)
        bad = self.spend(outpoint, ctv_script, control, [CTxOut(1_000_000 - FEE - 1, dest)])
        self.reject(bad, "template hash mismatch")
        self.accept(self.spend(outpoint, ctv_script, control, [CTxOut(1_000_000 - FEE, dest)]))

        self.log.info("Introspection: recursive pay-to-self vault")
        vault_script = CScript([0, OP_OUTPUTBYTECODE, OP_INPUTINDEX, OP_UTXOBYTECODE, OP_EQUALVERIFY,
                                0, OP_OUTPUTVALUE, OP_INPUTINDEX, OP_UTXOVALUE, FEE, OP_SUB, OP_GREATERTHANOREQUAL])
        spk, control = self.taproot(vault_script)
        outpoint = self.fund(spk, 2_000_000)
        self.reject(self.spend(outpoint, vault_script, control, [CTxOut(2_000_000 - FEE, dest)]), "EQUALVERIFY")
        self.reject(self.spend(outpoint, vault_script, control, [CTxOut(2_000_000 - FEE - 1, spk)]), "false/empty top stack element")
        again = self.spend(outpoint, vault_script, control, [CTxOut(2_000_000 - FEE, spk)])
        self.accept(again)
        assert_equal(self.node.gettxout(again.rehash(), 0)["scriptPubKey"]["hex"], spk.hex())

        self.log.info("OP_CAT: sha256(a || b) lock")
        a, b = b"aperture", b"science"
        cat_script = CScript([a, OP_SWAP, OP_CAT, OP_SHA256, sha256(a + b).digest(), OP_EQUAL])
        spk, control = self.taproot(cat_script)
        outpoint = self.fund(spk, 1_000_000)
        self.reject(self.spend(outpoint, cat_script, control, [CTxOut(1_000_000 - FEE, dest)], stack=[b"wrong"]),
                    "false/empty top stack element")
        self.accept(self.spend(outpoint, cat_script, control, [CTxOut(1_000_000 - FEE, dest)], stack=[b]))

        self.log.info("Token introspection: at most 100 tokens leave per spend")
        category = self.node.tokengenesis(self.node.getnewaddress(), 1000)["category"]
        self.node.generatetoaddress(1, self.addr)
        token_utxo = self.node.listtokens()["outputs"][0]
        limit_script = CScript([0, OP_OUTPUTTOKENCATEGORY, OP_INPUTINDEX, OP_UTXOTOKENCATEGORY, OP_EQUALVERIFY,
                                0, OP_OUTPUTTOKENAMOUNT, OP_INPUTINDEX, OP_UTXOTOKENAMOUNT, 100, OP_SUB, OP_GREATERTHANOREQUAL])
        spk, control = self.taproot(limit_script)
        outpoint = self.fund(spk, 100_000, encode_token_prefix(category, amount=1000),
                             extra_input=(token_utxo["txid"], token_utxo["vout"], int(token_utxo["value"] * 10**8)))
        keep = lambda n: CTxOut(40_000, CScript(encode_token_prefix(category, amount=n) + bytes(spk)))
        send = lambda n: CTxOut(40_000, CScript(encode_token_prefix(category, amount=n) + bytes(dest)))
        self.reject(self.spend(outpoint, limit_script, control, [keep(800), send(200)]), "false/empty top stack element")
        self.accept(self.spend(outpoint, limit_script, control, [keep(900), send(100)]))
        assert_equal(self.node.listtokens()["balances"][category]["amount"], "100")

        self.log.info("Introspection opcodes stay invalid outside tapscript (P2WSH)")
        witness_script = CScript([OP_INPUTINDEX, OP_TRUE])
        p2wsh = CScript([0, sha256(bytes(witness_script)).digest()])
        outpoint = self.fund(p2wsh, 1_000_000)
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(outpoint[0], 16), outpoint[1]))]
        tx.vout = [CTxOut(1_000_000 - FEE, dest)]
        tx.wit.vtxinwit = [CTxInWitness()]
        tx.wit.vtxinwit[0].scriptWitness.stack = [bytes(witness_script)]
        self.reject(tx, "Opcode missing or not understood")


if __name__ == '__main__':
    CovenantTest().main()
