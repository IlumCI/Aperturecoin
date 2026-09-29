# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""HTTP 402 pay-for-result flow on top of ServicePayment (doc/agent-payments.md).

Provider                                   Agent (payer)
--------                                   -------------
result = serve(request)
K = random 32 bytes
402 {amount, payee, key_hash=sha256(K),
     timeout, ciphertext=Enc_K(result)} -->
                                           fund ServicePayment(payer, payee, key_hash)
                                    <--    POST {txid, vout}
settle(): spend with K (K is now public)
                                           recover(): read K from the chain, decrypt

The provider is paid only by publishing K, and publishing K hands over the
result: payment and delivery are atomic. If the provider never settles, the
agent refunds after `timeout` blocks.
"""

import hashlib
import os
from dataclasses import dataclass

from .payments import ServicePayment, keystream_decrypt, keystream_encrypt, revealed_key


@dataclass
class Quote:
    amount: int
    payee_xonly: bytes
    key_hash: bytes
    timeout: int
    ciphertext: bytes

    def to_json(self):
        return {"amount": self.amount, "payee": self.payee_xonly.hex(), "key_hash": self.key_hash.hex(),
                "timeout": self.timeout, "ciphertext": self.ciphertext.hex()}

    @staticmethod
    def from_json(obj):
        return Quote(obj["amount"], bytes.fromhex(obj["payee"]), bytes.fromhex(obj["key_hash"]),
                     obj["timeout"], bytes.fromhex(obj["ciphertext"]))


def make_quote(result, amount, payee_xonly, timeout, key=None):
    """Provider side: returns (quote, key). Keep the key secret until settling."""
    key = key or os.urandom(32)
    return Quote(amount, payee_xonly, hashlib.sha256(key).digest(), timeout, keystream_encrypt(key, result)), key


def contract_for(quote, payer_xonly):
    return ServicePayment(payer_xonly, quote.payee_xonly, quote.key_hash, quote.timeout)


def recover(quote, settle_witness_stack):
    """Agent side: decrypt the result with the key revealed by the settle transaction."""
    key = revealed_key(settle_witness_stack)
    if hashlib.sha256(key).digest() != quote.key_hash:
        raise ValueError("revealed key does not match the quote")
    return keystream_decrypt(key, quote.ciphertext)
