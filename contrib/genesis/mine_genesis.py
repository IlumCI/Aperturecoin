#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Build and mine an ApertureCoin genesis block.

Mirrors CreateGenesisBlock() in src/chainparams.cpp: prints the merkle root,
nonce, block hash and PoW hash for the given parameters. Mining uses the
native helper built from mine_genesis.cpp (see README.md).
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "test", "functional"))
from test_framework.blake3 import blake3  # noqa: E402
from test_framework.aperture_matmulpow import getPoWHash  # noqa: E402


def sha256d(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def ser_compact_size(n):
    if n < 253:
        return bytes([n])
    if n <= 0xffff:
        return b"\xfd" + struct.pack("<H", n)
    return b"\xfe" + struct.pack("<I", n)


def push_data(b):
    if len(b) < 0x4c:
        return bytes([len(b)]) + b
    if len(b) <= 0xff:
        return b"\x4c" + bytes([len(b)]) + b
    return b"\x4d" + struct.pack("<H", len(b)) + b


def coinbase_tx(timestamp, output_script, reward):
    # scriptSig = CScript() << 486604799 << CScriptNum(4) << timestamp
    script_sig = push_data(struct.pack("<I", 486604799)) + push_data(b"\x04") + push_data(timestamp.encode())
    tx = struct.pack("<i", 1)
    tx += ser_compact_size(1) + b"\x00" * 32 + struct.pack("<I", 0xffffffff)
    tx += ser_compact_size(len(script_sig)) + script_sig + struct.pack("<I", 0xffffffff)
    tx += ser_compact_size(1) + struct.pack("<q", reward)
    tx += ser_compact_size(len(output_script)) + output_script
    tx += struct.pack("<I", 0)
    return tx


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--timestamp", required=True)
    p.add_argument("--script", required=True, help="genesis output scriptPubKey (hex)")
    p.add_argument("--time", type=int, required=True)
    p.add_argument("--bits", required=True, help="compact target, hex")
    p.add_argument("--dim", type=int, required=True)
    p.add_argument("--reward", type=int, default=50 * 100000000)
    p.add_argument("--miner", default=os.path.join(os.path.dirname(__file__), "mine_genesis"))
    p.add_argument("--threads", type=int, default=os.cpu_count())
    args = p.parse_args()

    tx = coinbase_tx(args.timestamp, bytes.fromhex(args.script), args.reward)
    merkle = sha256d(tx)
    bits = int(args.bits, 16)
    prefix = struct.pack("<i", 1) + b"\x00" * 32 + merkle + struct.pack("<II", args.time, bits)
    out = subprocess.run([args.miner, prefix.hex(), args.bits, str(args.dim), str(args.threads)],
                         check=True, capture_output=True, text=True)
    nonce = int(out.stdout.strip())
    header = prefix + struct.pack("<I", nonce)
    pow_hash = getPoWHash(header, args.dim) if args.dim <= 64 else None
    print("merkle_root", merkle[::-1].hex())
    print("nonce", nonce)
    print("block_hash", sha256d(header)[::-1].hex())
    if pow_hash is not None:
        print("pow_hash", pow_hash[::-1].hex())


if __name__ == "__main__":
    main()
