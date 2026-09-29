# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Agent mandates: spending policies an agent key cannot exceed, enforced by consensus.

A mandate is a Taproot output (the vault) that carries a mutable NFT whose
commitment holds the mandate state. See doc/agent-mandates.md.

- Key path: the owner's key (sweep, top up, revoke) at any time.
- Script paths: one leaf per allowlisted destination. A leaf lets the agent
  key pay that destination if all of the following hold:
    * the amount fits the remaining budget of the current period;
    * a new period (full budget) may start only when period_start + period
      <= new_start <= tx locktime (CLTV), and only while new_start < expiry;
    * output 1 continues the vault: same script, same mandate NFT (mutable),
      commitment updated to the new state, value >= input - amount - max_fee;
    * output 0 pays exactly `amount` to the destination.

State commitment: bytes([len(remaining) + 1]) || remaining || period_start,
with both numbers minimally encoded; the length byte makes it unambiguous.
"""

from dataclasses import dataclass, field
from typing import List

from test_framework.key import compute_xonly_pubkey, sign_schnorr, tweak_add_privkey
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut
from test_framework.script import (
    CScript,
    OP_0, OP_1, OP_1ADD, OP_2, OP_ADD, OP_2DUP, OP_3, OP_CAT, OP_CHECKLOCKTIMEVERIFY, OP_CHECKSIG,
    OP_DROP, OP_DUP, OP_ELSE, OP_ENDIF, OP_EQUALVERIFY, OP_GREATERTHAN, OP_GREATERTHANOREQUAL,
    OP_IF, OP_INPUTINDEX, OP_LESSTHAN, OP_LESSTHANOREQUAL, OP_NUMEQUALVERIFY, OP_OUTPUTBYTECODE,
    OP_OUTPUTTOKENCATEGORY, OP_OUTPUTTOKENCOMMITMENT, OP_OUTPUTVALUE, OP_OVER, OP_PICK, OP_SIZE,
    OP_SUB, OP_SWAP, OP_UTXOBYTECODE, OP_UTXOTOKENCATEGORY, OP_UTXOTOKENCOMMITMENT, OP_UTXOVALUE,
    OP_VERIFY, TaprootSignatureHash, taproot_construct,
)
from test_framework.tokens import encode_token_prefix

from . import scriptnum


def encode_state(remaining, period_start):
    rem = scriptnum.encode(remaining)
    return bytes([len(rem) + 1]) + rem + scriptnum.encode(period_start)


def agent_leaf(agent_xonly, destination, budget, period, expiry, max_fee):
    """Tapscript for one allowlisted destination (witness: sig new_start amount rem_old start_old)."""
    return CScript([
        # Old state must match the mandate NFT commitment of the spent vault.
        OP_2DUP, OP_SWAP, OP_SIZE, OP_1ADD, OP_SWAP, OP_CAT, OP_SWAP, OP_CAT,
        OP_INPUTINDEX, OP_UTXOTOKENCOMMITMENT, OP_EQUALVERIFY,
        # Budget available in this spend.
        OP_3, OP_PICK, OP_OVER, OP_GREATERTHAN,
        OP_IF,
            # New period: start_old + period <= new_start < expiry, new_start <= locktime.
            period, OP_ADD,
            OP_3, OP_PICK, OP_LESSTHANOREQUAL, OP_VERIFY,
            OP_DROP,
            OP_OVER, expiry, OP_LESSTHAN, OP_VERIFY,
            OP_OVER, OP_CHECKLOCKTIMEVERIFY, OP_DROP,
            budget,
        OP_ELSE,
            # Same period: new_start == start_old; available = remaining.
            OP_3, OP_PICK, OP_NUMEQUALVERIFY,
        OP_ENDIF,
        # new_remaining = available - amount >= 0
        OP_OVER, OP_SUB, OP_DUP, 0, OP_GREATERTHANOREQUAL, OP_VERIFY,
        # Output 1 carries the new state.
        OP_SIZE, OP_1ADD, OP_SWAP, OP_CAT, OP_2, OP_PICK, OP_CAT,
        OP_1, OP_OUTPUTTOKENCOMMITMENT, OP_EQUALVERIFY,
        # Output 1 continues the vault (script, mutable mandate NFT, value).
        OP_1, OP_OUTPUTBYTECODE, OP_INPUTINDEX, OP_UTXOBYTECODE, OP_EQUALVERIFY,
        OP_1, OP_OUTPUTTOKENCATEGORY, OP_INPUTINDEX, OP_UTXOTOKENCATEGORY, OP_EQUALVERIFY,
        OP_1, OP_OUTPUTVALUE, OP_INPUTINDEX, OP_UTXOVALUE, OP_2, OP_PICK, OP_SUB, max_fee, OP_SUB,
        OP_GREATERTHANOREQUAL, OP_VERIFY,
        # Output 0 pays exactly `amount` to the allowlisted destination.
        OP_0, OP_OUTPUTBYTECODE, bytes(destination), OP_EQUALVERIFY,
        OP_0, OP_OUTPUTVALUE, OP_NUMEQUALVERIFY,
        OP_DROP,
        agent_xonly, OP_CHECKSIG,
    ])


@dataclass
class Mandate:
    owner_xonly: bytes
    agent_xonly: bytes
    budget: int          # satoshis per period
    period: int          # blocks
    expiry: int          # no new period may start at or after this height
    allowlist: List[bytes]  # destination locking bytecodes
    max_fee: int = 50_000
    category: str = ""   # mandate NFT category (hex, RPC byte order), set at genesis
    leaves: dict = field(default_factory=dict, init=False)

    def __post_init__(self):
        scripts = [(f"dest{i}", agent_leaf(self.agent_xonly, d, self.budget, self.period, self.expiry, self.max_fee))
                   for i, d in enumerate(self.allowlist)]
        self.info = taproot_construct(self.owner_xonly, scripts)

    @property
    def script_pubkey(self):
        return self.info.scriptPubKey

    def vault_output(self, value, remaining, period_start):
        prefix = encode_token_prefix(self.category, nft="mutable", commitment=encode_state(remaining, period_start))
        return CTxOut(value, CScript(prefix + bytes(self.script_pubkey)))

    def _leaf_for(self, destination):
        index = self.allowlist.index(bytes(destination))
        leaf = self.info.leaves[f"dest{index}"]
        control = bytes([leaf.version + self.info.negflag]) + self.info.inner_pubkey + leaf.merklebranch
        return leaf, control

    def spend(self, vault_utxo, destination, amount, fee, *, remaining, period_start, new_start, locktime):
        """Unsigned agent spend. vault_utxo = (txid_hex, vout, value)."""
        new_remaining = (self.budget if new_start != period_start else remaining) - amount
        tx = CTransaction()
        tx.nVersion = 2
        tx.nLockTime = locktime
        tx.vin = [CTxIn(COutPoint(int(vault_utxo[0], 16), vault_utxo[1]), b"", 0xfffffffe)]
        tx.vout = [CTxOut(amount, CScript(bytes(destination))),
                   self.vault_output(vault_utxo[2] - amount - fee, new_remaining, new_start)]
        tx.wit.vtxinwit = [CTxInWitness()]
        return tx, new_remaining

    def sign_agent(self, tx, spent_output, agent_privkey, destination, *, remaining, period_start, new_start, amount):
        leaf, control = self._leaf_for(destination)
        sighash = TaprootSignatureHash(tx, [spent_output], 0, 0, scriptpath=True, script=leaf.script)
        sig = sign_schnorr(agent_privkey, sighash)
        tx.wit.vtxinwit[0].scriptWitness.stack = [
            sig, scriptnum.encode(new_start), scriptnum.encode(amount),
            scriptnum.encode(remaining), scriptnum.encode(period_start),
            bytes(leaf.script), control,
        ]
        tx.rehash()
        return tx

    def sweep(self, vault_utxo, spent_output, owner_privkey, destination, fee):
        """Owner key-path spend of the whole vault (the mandate NFT is burned)."""
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(vault_utxo[0], 16), vault_utxo[1]))]
        tx.vout = [CTxOut(vault_utxo[2] - fee, CScript(bytes(destination)))]
        tx.wit.vtxinwit = [CTxInWitness()]
        tweaked = tweak_add_privkey(owner_privkey, self.info.tweak)
        sighash = TaprootSignatureHash(tx, [spent_output], 0, 0)
        tx.wit.vtxinwit[0].scriptWitness.stack = [sign_schnorr(tweaked, sighash)]
        tx.rehash()
        return tx


def xonly(privkey):
    return compute_xonly_pubkey(privkey)[0]
