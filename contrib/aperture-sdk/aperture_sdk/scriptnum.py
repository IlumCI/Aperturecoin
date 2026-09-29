# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Minimal script number encoding (as pushed on the stack, without opcode)."""


def encode(n):
    """Minimal little-endian sign-magnitude encoding (CScriptNum::serialize)."""
    if n == 0:
        return b""
    neg = n < 0
    absval = -n if neg else n
    out = bytearray()
    while absval:
        out.append(absval & 0xff)
        absval >>= 8
    if out[-1] & 0x80:
        out.append(0x80 if neg else 0x00)
    elif neg:
        out[-1] |= 0x80
    return bytes(out)
