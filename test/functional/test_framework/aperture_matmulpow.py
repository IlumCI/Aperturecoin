#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Reference implementation of the ApertureMatMul v1 proof-of-work.

See doc/matmulpow.md. The matrix dimension is a per-network consensus
parameter; the functional tests run on regtest (n = 32).
"""

import struct

from .blake3 import blake3

TAG = b"ApertureMatMul/v1"
REGTEST_DIM = 32


def _to_int8(buf):
    return [b - 256 if b > 127 else b for b in buf]


def matmul_digest(header, n):
    """Return BLAKE3 of C = A*B (int32 LE, row-major) for the given header."""
    stream = blake3(TAG + header, 2 * n * n)
    a = _to_int8(stream[:n * n])
    b = _to_int8(stream[n * n:])
    cols = [b[j::n] for j in range(n)]
    out = bytearray()
    for i in range(n):
        row = a[i * n:(i + 1) * n]
        out += struct.pack("<%di" % n, *[sum(x * y for x, y in zip(row, col)) for col in cols])
    return blake3(bytes(out))


def getPoWHash(header, n=REGTEST_DIM):
    """Return the 32-byte ApertureMatMul v1 proof-of-work hash of an 80-byte header."""
    if not isinstance(header, (bytes, bytearray, memoryview)):
        raise TypeError("header must be a bytes-like object")
    header = bytes(header)
    if len(header) != 80:
        raise ValueError(f"expected 80-byte block header, got {len(header)}")
    return blake3(header + matmul_digest(header, n))
