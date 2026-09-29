#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Native token prefix encoding (src/primitives/token.h, CashTokens CHIP-2022-02)."""

from .messages import ser_compact_size

PREFIX_TOKEN = 0xef
HAS_COMMITMENT_LENGTH = 0x40
HAS_NFT = 0x20
HAS_AMOUNT = 0x10
CAPABILITIES = {"none": 0, "mutable": 1, "minting": 2}


def encode_token_prefix(category_hex, amount=0, nft=None, commitment=b"", bitfield_override=None):
    """Return the token prefix bytes. category_hex is the display (RPC) hex of the category."""
    bitfield = 0
    body = b""
    if nft is not None:
        bitfield |= HAS_NFT | CAPABILITIES[nft]
        if commitment:
            bitfield |= HAS_COMMITMENT_LENGTH
            body += ser_compact_size(len(commitment)) + commitment
    if amount:
        bitfield |= HAS_AMOUNT
        body += ser_compact_size(amount)
    if bitfield_override is not None:
        bitfield = bitfield_override
    return bytes([PREFIX_TOKEN]) + bytes.fromhex(category_hex)[::-1] + bytes([bitfield]) + body


def token_script(category_hex, locking_script_hex, **kwargs):
    """Full scriptPubKey hex: token prefix followed by the locking bytecode."""
    return (encode_token_prefix(category_hex, **kwargs) + bytes.fromhex(locking_script_hex)).hex()
