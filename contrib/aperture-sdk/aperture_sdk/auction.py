# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Frequent batch auctions with a uniform clearing price (doc/batch-auctions.md).

An order is a Taproot output whose parameters stay sealed (hidden in a
tapleaf) until settlement. Its "fill" leaf reads the clearing price p from the
settlement transaction's last output, a marker

    OP_RETURN <"APXP" || category> <p>

and enforces, for input i and the output with the same index i:
  sell (q tokens):   p >= limit, output i pays the owner exactly q*p + the
                     order's SCIENCE value, with no tokens
  buy  (V SCIENCE):  p <= limit, output i pays the owner exactly q tokens of
                     the category plus V - q*p SCIENCE

Every order in a settlement reads the same marker, so all fills clear at one
price: covenants enforce uniform-price batch execution without a dedicated
consensus rule. The "cancel" leaf lets the owner reclaim the order at any time.

Prices are integers in satoshis per token base unit; choose the token's base
unit accordingly.
"""

from test_framework.key import sign_schnorr
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut
from test_framework.script import (
    CScript, OP_1SUB, OP_CAT, OP_CHECKSIG, OP_DUP, OP_EQUALVERIFY, OP_GREATERTHANOREQUAL,
    OP_INPUTINDEX, OP_LESSTHANOREQUAL, OP_MUL, OP_NUMEQUAL, OP_NUMEQUALVERIFY, OP_OUTPUTBYTECODE,
    OP_OUTPUTTOKENAMOUNT, OP_OUTPUTTOKENCATEGORY, OP_OUTPUTVALUE, OP_RETURN, OP_SIZE, OP_SUB, OP_SWAP,
    OP_ADD, OP_TXOUTPUTCOUNT, OP_UTXOVALUE, OP_VERIFY, TaprootSignatureHash, taproot_construct,
)

from . import scriptnum
from .payments import NUMS_XONLY

TAG = b"APXP"


def marker_prefix(category_hex):
    payload = TAG + bytes.fromhex(category_hex)[::-1]
    return bytes([OP_RETURN, len(payload)]) + payload


def marker_script(category_hex, price):
    p = scriptnum.encode(price)
    return CScript(marker_prefix(category_hex) + bytes([len(p)]) + p)


def _marker_check(category_hex):
    # stack: p -> p (verifies the last output is the marker for p)
    return [OP_DUP, OP_SIZE, OP_SWAP, OP_CAT, marker_prefix(category_hex), OP_SWAP, OP_CAT,
            OP_TXOUTPUTCOUNT, OP_1SUB, OP_OUTPUTBYTECODE, OP_EQUALVERIFY]


class Order:
    def __init__(self, side, category_hex, quantity, limit, owner_xonly, owner_spk):
        assert side in ("buy", "sell")
        self.side, self.category, self.quantity, self.limit = side, category_hex, quantity, limit
        self.owner_spk = bytes(owner_spk)
        if side == "sell":
            fill = CScript([
                OP_DUP, limit, OP_GREATERTHANOREQUAL, OP_VERIFY,
                *_marker_check(category_hex),
                OP_INPUTINDEX, OP_OUTPUTBYTECODE, self.owner_spk, OP_EQUALVERIFY,
                quantity, OP_MUL, OP_INPUTINDEX, OP_UTXOVALUE, OP_ADD,
                OP_INPUTINDEX, OP_OUTPUTVALUE, OP_NUMEQUALVERIFY,
                OP_INPUTINDEX, OP_OUTPUTTOKENAMOUNT, 0, OP_NUMEQUAL,
            ])
        else:
            fill = CScript([
                OP_DUP, limit, OP_LESSTHANOREQUAL, OP_VERIFY,
                *_marker_check(category_hex),
                OP_INPUTINDEX, OP_OUTPUTBYTECODE, self.owner_spk, OP_EQUALVERIFY,
                OP_INPUTINDEX, OP_OUTPUTTOKENCATEGORY, bytes.fromhex(category_hex)[::-1], OP_EQUALVERIFY,
                OP_INPUTINDEX, OP_OUTPUTTOKENAMOUNT, quantity, OP_NUMEQUALVERIFY,
                quantity, OP_MUL, OP_INPUTINDEX, OP_UTXOVALUE, OP_SWAP, OP_SUB,
                OP_INPUTINDEX, OP_OUTPUTVALUE, OP_NUMEQUAL,
            ])
        cancel = CScript([owner_xonly, OP_CHECKSIG])
        self.info = taproot_construct(NUMS_XONLY, [("fill", fill), ("cancel", cancel)])

    @property
    def script_pubkey(self):
        return self.info.scriptPubKey

    def control(self, name):
        leaf = self.info.leaves[name]
        return leaf, bytes([leaf.version + self.info.negflag]) + self.info.inner_pubkey + leaf.merklebranch

    def fill_witness(self, price):
        leaf, control = self.control("fill")
        return [scriptnum.encode(price), bytes(leaf.script), control]

    def cancel(self, outpoint, spent_output, owner_privkey, destination, fee):
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(outpoint[0], 16), outpoint[1]))]
        tx.vout = [CTxOut(spent_output.nValue - fee, CScript(bytes(destination)))]
        leaf, control = self.control("cancel")
        sighash = TaprootSignatureHash(tx, [spent_output], 0, 0, scriptpath=True, script=leaf.script)
        tx.wit.vtxinwit = [CTxInWitness()]
        tx.wit.vtxinwit[0].scriptWitness.stack = [sign_schnorr(owner_privkey, sighash), bytes(leaf.script), control]
        tx.rehash()
        return tx


def clearing_price(orders):
    """Uniform price maximising matched volume (sells ascending, buys descending).

    Returns (price, matched sells, matched buys) for all-or-nothing orders; the
    price is the midpoint of the marginal limits, rounded down.
    """
    sells = sorted((o for o in orders if o.side == "sell"), key=lambda o: o.limit)
    buys = sorted((o for o in orders if o.side == "buy"), key=lambda o: -o.limit)
    best = (0, None, [], [])
    for i in range(1, len(sells) + 1):
        for j in range(1, len(buys) + 1):
            s, b = sells[:i], buys[:j]
            if sum(o.quantity for o in s) != sum(o.quantity for o in b):
                continue
            lo, hi = s[-1].limit, b[-1].limit
            if lo <= hi and sum(o.quantity for o in s) > best[0]:
                best = (sum(o.quantity for o in s), (lo + hi) // 2, s, b)
    return best[1], best[2], best[3]
