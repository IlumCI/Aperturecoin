#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""ApertureMatMul v2 tickets in pure Python (doc/pouw-v2.md).

An independent implementation of src/crypto/matmulpow_v2.cpp, used to mine
v2 blocks in functional tests. The activation panel is not authenticated by
consensus, so a test miner may use any panel (e.g. zeros) over the real
protocol-model weights, which it reads straight from the tiny model's BLAKE3
stream at the op's offset.
"""

import struct

from .blake3 import blake3

GROUP = 256
QMAX = 95
SEED_TAG = b"ApertureMatMul/v2/seed"
NOISE_TAG = b"ApertureMatMul/v2/noise"
BATCH_TAG = b"ApertureBatch/v0"
EL, ER, FL, FR = 0, 1, 2, 3

# tiny_model(seed=1): H, L, NH, KV, D, FF, V = 256, 2, 2, 1, 128, 768, 257
_H, _L, _NH, _KV, _D, _FF, _V = 256, 2, 2, 1, 128, 768, 257
_SHAPES = [(_NH * _D, _H), (_KV * _D, _H), (_KV * _D, _H), (_H, _NH * _D), (_FF, _H), (_FF, _H), (_H, _FF)]


def tiny_op(op, seed=1):
    """(d_in, d_out, weights row-major d_out x d_in) of a tiny-model weight matmul."""
    xof = b"ApertureTinyModel/v0" + struct.pack("<I", seed)
    off = _V * _H + 4 * _V
    for layer in range(_L):
        off += 4 * (_H + _H + _D + _D)
        for p, (dout, din) in enumerate(_SHAPES):
            if layer * 7 + p == op:
                raw = blake3(xof, dout * din, off)
                return din, dout, [(b % (2 * QMAX + 1)) - QMAX for b in raw]
            off += dout * din + 4 * dout
    raise ValueError("op out of range")


def seed(header80, batch_root):
    return blake3(SEED_TAG + header80 + batch_root)


def noise(sigma, op, factor, offset, length):
    raw = blake3(NOISE_TAG + sigma + struct.pack("<HB", op, factor), length, offset)
    return [(b % 3) - 1 for b in raw]


def fold(p):
    lanes = [0, 0, 0, 0]
    for k, v in enumerate(p):
        x = (v & 0xffffffff) ^ k
        y = ((x ^ (x >> 15)) * 0x2c1b3c6d) & 0xffffffff
        m = y ^ (y >> 12)
        rot = k % 32
        m = ((m << rot) | (m >> (32 - rot))) & 0xffffffff if rot else m
        lanes[k % 4] = (lanes[k % 4] + m) & 0xffffffff
    return struct.pack("<4I", *lanes)


def ticket_pow(sigma, r, op, op_desc, i, j, s, panel):
    """pow hash (int, little-endian) of ticket (op, i, j, s) over an r x 256 int8 panel."""
    din, dout, w = op_desc
    G = GROUP
    el = noise(sigma, op, EL, i * r * r, r * r)
    er = sum((noise(sigma, op, ER, k * din + s * G, G) for k in range(r)), [])
    fl = noise(sigma, op, FL, s * G * r, G * r)
    fr = sum((noise(sigma, op, FR, k * dout + j * r, r) for k in range(r)), [])
    ap = [[panel[a * G + c] + sum(el[a * r + k] * er[k * G + c] for k in range(r)) for c in range(G)] for a in range(r)]
    wp = [[w[(j * r + c) * din + s * G + k] + sum(fl[k * r + q] * fr[q * r + c] for q in range(r)) for c in range(r)] for k in range(G)]
    p = []
    for a in range(r):
        for c in range(r):
            p.append(sum(ap[a][k] * wp[k][c] for k in range(G)))
    digest = fold([v if v >= 0 else v + (1 << 32) for v in p])
    h = blake3(sigma + struct.pack("<4H", op, i, j, s) + digest)
    return int.from_bytes(h, "little")


def batch_root(outpoints):
    """outpoints: [(txid int, vout)] in block order -> 32 raw bytes."""
    data = BATCH_TAG
    for txid, n in outpoints:
        data += txid.to_bytes(32, "little") + struct.pack("<I", n)
    return blake3(data)


def target_from_bits(bits):
    exp, mant = bits >> 24, bits & 0x7fffff
    return mant << (8 * (exp - 3)) if exp >= 3 else mant >> (8 * (3 - exp))



def ser_extension(root, op, i, j, s, panel):
    from .messages import ser_compact_size
    return root + struct.pack("<4H", op, i, j, s) + ser_compact_size(len(panel)) + bytes(v & 0xff for v in panel)


def solve(block, r, root, op=0, panel=None):
    """Mine a v2 header for `block` (a messages.CBlock) with a zero (or given) panel."""
    from .messages import CBlockHeader
    block.nVersion |= CBlockHeader.VERSION_POWV2
    panel = panel if panel is not None else [0] * (r * GROUP)
    op_desc = tiny_op(op)
    target = target_from_bits(block.nBits)
    while True:
        header80 = struct.pack("<i", block.nVersion) + block.hashPrevBlock.to_bytes(32, "little") + \
            block.hashMerkleRoot.to_bytes(32, "little") + struct.pack("<III", block.nTime, block.nBits, block.nNonce)
        sigma = seed(header80, root)
        for j in range(op_desc[1] // r):
            if ticket_pow(sigma, r, op, op_desc, 0, j, 0, panel) <= target:
                block.powv2 = ser_extension(root, op, 0, j, 0, panel)
                block.rehash()
                return
        block.nNonce += 1
