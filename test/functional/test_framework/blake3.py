#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Pure-Python BLAKE3 (hash mode with extendable output).

Reference: https://github.com/BLAKE3-team/BLAKE3-specs. Used by the
functional test framework to compute ApertureMatMul proof-of-work hashes.
If the optional `blake3` C extension is installed it is used instead.
"""

import struct

try:
    import blake3 as _blake3_ext  # type: ignore
except ImportError:  # pragma: no cover
    _blake3_ext = None

IV = (0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
      0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19)
MSG_PERMUTATION = (2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8)
CHUNK_START = 1 << 0
CHUNK_END = 1 << 1
PARENT = 1 << 2
ROOT = 1 << 3
BLOCK_LEN = 64
CHUNK_LEN = 1024
M32 = 0xFFFFFFFF


def _rotr(x, n):
    return ((x >> n) | (x << (32 - n))) & M32


def _g(s, a, b, c, d, mx, my):
    s[a] = (s[a] + s[b] + mx) & M32
    s[d] = _rotr(s[d] ^ s[a], 16)
    s[c] = (s[c] + s[d]) & M32
    s[b] = _rotr(s[b] ^ s[c], 12)
    s[a] = (s[a] + s[b] + my) & M32
    s[d] = _rotr(s[d] ^ s[a], 8)
    s[c] = (s[c] + s[d]) & M32
    s[b] = _rotr(s[b] ^ s[c], 7)


def _compress(cv, block_words, counter, block_len, flags):
    s = list(cv) + list(IV[:4]) + [counter & M32, (counter >> 32) & M32, block_len, flags]
    m = list(block_words)
    for r in range(7):
        _g(s, 0, 4, 8, 12, m[0], m[1])
        _g(s, 1, 5, 9, 13, m[2], m[3])
        _g(s, 2, 6, 10, 14, m[4], m[5])
        _g(s, 3, 7, 11, 15, m[6], m[7])
        _g(s, 0, 5, 10, 15, m[8], m[9])
        _g(s, 1, 6, 11, 12, m[10], m[11])
        _g(s, 2, 7, 8, 13, m[12], m[13])
        _g(s, 3, 4, 9, 14, m[14], m[15])
        if r < 6:
            m = [m[i] for i in MSG_PERMUTATION]
    for i in range(8):
        s[i] ^= s[i + 8]
        s[i + 8] ^= cv[i]
    return s


def _words(block):
    block = block + b"\x00" * (BLOCK_LEN - len(block))
    return struct.unpack("<16I", block)


class _Output:
    def __init__(self, cv, block_words, counter, block_len, flags):
        self.cv, self.block_words, self.counter = cv, block_words, counter
        self.block_len, self.flags = block_len, flags

    def chaining_value(self):
        return _compress(self.cv, self.block_words, self.counter, self.block_len, self.flags)[:8]

    def root_bytes(self, length):
        out = bytearray()
        counter = 0
        while len(out) < length:
            words = _compress(self.cv, self.block_words, counter, self.block_len, self.flags | ROOT)
            out += struct.pack("<16I", *words)
            counter += 1
        return bytes(out[:length])


def _chunk_output(chunk, chunk_counter):
    cv = IV
    blocks = [chunk[i:i + BLOCK_LEN] for i in range(0, len(chunk), BLOCK_LEN)] or [b""]
    for idx, block in enumerate(blocks):
        flags = CHUNK_START if idx == 0 else 0
        if idx == len(blocks) - 1:
            return _Output(cv, _words(block), chunk_counter, len(block), flags | CHUNK_END)
        cv = _compress(cv, _words(block), chunk_counter, BLOCK_LEN, flags)[:8]


def _parent_output(left_cv, right_cv):
    return _Output(IV, tuple(left_cv) + tuple(right_cv), 0, BLOCK_LEN, PARENT)


def blake3(data, length=32):
    """Return BLAKE3(data) with `length` bytes of output (XOF)."""
    data = bytes(data)
    if _blake3_ext is not None:
        return _blake3_ext.blake3(data).digest(length=length)
    chunks = [data[i:i + CHUNK_LEN] for i in range(0, len(data), CHUNK_LEN)] or [b""]
    stack = []
    for counter, chunk in enumerate(chunks[:-1]):
        cv = _chunk_output(chunk, counter).chaining_value()
        total = counter + 1
        while total & 1 == 0:
            cv = _parent_output(stack.pop(), cv).chaining_value()
            total >>= 1
        stack.append(cv)
    output = _chunk_output(chunks[-1], len(chunks) - 1)
    while stack:
        output = _parent_output(stack.pop(), output.chaining_value())
    return output.root_bytes(length)
