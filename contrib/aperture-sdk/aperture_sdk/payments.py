# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Agent payments: atomic pay-for-result and refundable escrow (doc/agent-payments.md).

Both are Taproot outputs with a provably unspendable internal key (BIP341 NUMS
point H), so only their script paths can spend them.

Atomic service payment (A402-style, arXiv 2603.01179):
  settle: OP_SHA256 <sha256(K)> OP_EQUALVERIFY <payee> OP_CHECKSIG
  refund: <timeout> OP_CHECKSEQUENCEVERIFY OP_DROP <payer> OP_CHECKSIG
The provider encrypts the service result under key K and quotes sha256(K).
Claiming the payment publishes K on chain, so the payer can decrypt the result:
the payment and the delivery of the result are atomic.

Refundable escrow (authorize, then capture; arXiv 2609.02208):
  capture: output 1 pays the payer, outputs 0 + 1 >= input - fee_cap, merchant signs
  refund:  <timeout> OP_CHECKSEQUENCEVERIFY OP_DROP <payer> OP_CHECKSIG
The merchant may capture any part of the authorization but must return the
rest to the payer; after the timeout the payer can take everything back.
"""

import hashlib

from test_framework.key import sign_schnorr
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut
from test_framework.script import (
    CScript, OP_0, OP_1, OP_ADD, OP_CHECKSEQUENCEVERIFY, OP_CHECKSIG, OP_DROP, OP_EQUALVERIFY,
    OP_GREATERTHANOREQUAL, OP_INPUTINDEX, OP_OUTPUTBYTECODE, OP_OUTPUTVALUE, OP_SHA256, OP_SUB,
    OP_UTXOVALUE, OP_VERIFY, TaprootSignatureHash, taproot_construct,
)

# BIP341 "H": x-only point with no known discrete logarithm.
NUMS_XONLY = bytes.fromhex("50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0")


class _TaprootContract:
    def __init__(self, leaves):
        self.info = taproot_construct(NUMS_XONLY, leaves)

    @property
    def script_pubkey(self):
        return self.info.scriptPubKey

    def _control(self, name):
        leaf = self.info.leaves[name]
        return leaf, bytes([leaf.version + self.info.negflag]) + self.info.inner_pubkey + leaf.merklebranch

    @staticmethod
    def _tx(outpoint, outputs, sequence=0xffffffff):
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(outpoint[0], 16), outpoint[1]), b"", sequence)]
        tx.vout = outputs
        tx.wit.vtxinwit = [CTxInWitness()]
        return tx

    def _sign(self, tx, spent_output, name, privkey, extra_stack=()):
        leaf, control = self._control(name)
        sighash = TaprootSignatureHash(tx, [spent_output], 0, 0, scriptpath=True, script=leaf.script)
        tx.wit.vtxinwit[0].scriptWitness.stack = list(extra_stack) + [sign_schnorr(privkey, sighash), bytes(leaf.script), control]
        tx.rehash()
        return tx


class ServicePayment(_TaprootContract):
    """Atomic pay-for-result output."""

    def __init__(self, payer_xonly, payee_xonly, key_hash, timeout_blocks):
        self.key_hash = key_hash
        settle = CScript([OP_SHA256, key_hash, OP_EQUALVERIFY, payee_xonly, OP_CHECKSIG])
        refund = CScript([timeout_blocks, OP_CHECKSEQUENCEVERIFY, OP_DROP, payer_xonly, OP_CHECKSIG])
        self.timeout = timeout_blocks
        super().__init__([("settle", settle), ("refund", refund)])

    def settle(self, outpoint, spent_output, payee_privkey, key, destination, fee):
        """Payee claims by revealing K. Witness: <sig> <K> (K is consumed first by OP_SHA256)."""
        tx = self._tx(outpoint, [CTxOut(spent_output.nValue - fee, CScript(bytes(destination)))])
        leaf, control = self._control("settle")
        sighash = TaprootSignatureHash(tx, [spent_output], 0, 0, scriptpath=True, script=leaf.script)
        tx.wit.vtxinwit[0].scriptWitness.stack = [sign_schnorr(payee_privkey, sighash), key, bytes(leaf.script), control]
        tx.rehash()
        return tx

    def refund(self, outpoint, spent_output, payer_privkey, destination, fee):
        tx = self._tx(outpoint, [CTxOut(spent_output.nValue - fee, CScript(bytes(destination)))], sequence=self.timeout)
        return self._sign(tx, spent_output, "refund", payer_privkey)


def revealed_key(settle_witness_stack):
    """Extract K from a settle transaction's witness (payer side)."""
    return settle_witness_stack[1]


class Escrow(_TaprootContract):
    """Refundable authorization: the merchant captures part, the payer keeps the rest."""

    def __init__(self, payer_xonly, merchant_xonly, payer_spk, timeout_blocks, fee_cap):
        capture = CScript([
            OP_1, OP_OUTPUTBYTECODE, bytes(payer_spk), OP_EQUALVERIFY,
            OP_0, OP_OUTPUTVALUE, OP_1, OP_OUTPUTVALUE, OP_ADD,
            OP_INPUTINDEX, OP_UTXOVALUE, fee_cap, OP_SUB, OP_GREATERTHANOREQUAL, OP_VERIFY,
            merchant_xonly, OP_CHECKSIG,
        ])
        refund = CScript([timeout_blocks, OP_CHECKSEQUENCEVERIFY, OP_DROP, payer_xonly, OP_CHECKSIG])
        self.payer_spk = bytes(payer_spk)
        self.timeout = timeout_blocks
        super().__init__([("capture", capture), ("refund", refund)])

    def capture(self, outpoint, spent_output, merchant_privkey, merchant_spk, amount, fee, payer_spk=None):
        rest = spent_output.nValue - amount - fee
        tx = self._tx(outpoint, [CTxOut(amount, CScript(bytes(merchant_spk))),
                                 CTxOut(rest, CScript(payer_spk if payer_spk is not None else self.payer_spk))])
        return self._sign(tx, spent_output, "capture", merchant_privkey)

    def refund(self, outpoint, spent_output, payer_privkey, fee):
        tx = self._tx(outpoint, [CTxOut(spent_output.nValue - fee, CScript(self.payer_spk))], sequence=self.timeout)
        return self._sign(tx, spent_output, "refund", payer_privkey)


def keystream_encrypt(key, data):
    """Reference-only stream cipher (SHA-256 in counter mode) for the result envelope."""
    out = bytearray()
    counter = 0
    while len(out) < len(data):
        out += hashlib.sha256(key + counter.to_bytes(8, "little")).digest()
        counter += 1
    return bytes(a ^ b for a, b in zip(data, out))


keystream_decrypt = keystream_encrypt
