#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Frequent batch auctions with a uniform clearing price (doc/batch-auctions.md).

Sealed (Taproot) buy and sell orders for a token are settled in one
transaction at the price in its OP_RETURN marker. Every order's covenant
checks its own fill at exactly that price and within its limit, so:
- a settlement at the clearing price is valid;
- a price outside any matched order's limit is rejected;
- paying one trader at a different price than the marker (price
  discrimination) is rejected;
- owners can cancel unfilled orders.
"""

import os
import sys
from decimal import Decimal

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "contrib", "aperture-sdk"))
from aperture_sdk.auction import Order, clearing_price, marker_script  # noqa: E402
from aperture_sdk.mandate import xonly  # noqa: E402

from test_framework.key import generate_privkey  # noqa: E402
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut, FromHex, ToHex  # noqa: E402
from test_framework.script import CScript  # noqa: E402
from test_framework.test_framework import BitcoinTestFramework  # noqa: E402
from test_framework.tokens import encode_token_prefix  # noqa: E402
from test_framework.util import assert_equal  # noqa: E402

DUST = 10_000
BUY_FUNDS = 50_000


class BatchAuctionTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def spk(self):
        return bytes.fromhex(self.node.getaddressinfo(self.node.getnewaddress())["scriptPubKey"])

    def post_orders(self, outputs, token_input=None):
        """Create order outputs in one wallet-funded transaction; returns txid."""
        utxo = next(u for u in self.node.listunspent() if u["amount"] > 1)
        vin = [{"txid": utxo["txid"], "vout": utxo["vout"]}]
        total = int(utxo["amount"] * 10**8)
        if token_input:
            vin.append({"txid": token_input["txid"], "vout": token_input["vout"]})
            total += int(token_input["value"] * 10**8)
        change = Decimal(total - sum(o.nValue for o in outputs) - 100_000) / 10**8
        tx = FromHex(CTransaction(), self.node.createrawtransaction(vin, [{self.node.getnewaddress(): change}]))
        tx.vout += outputs
        txid = self.node.sendrawtransaction(self.node.signrawtransactionwithwallet(ToHex(tx))["hex"])
        self.node.generatetoaddress(1, self.addr)
        return txid

    def settlement(self, fills, price, payouts, fee=20_000):
        """fills: [(order, outpoint, value)], payouts: [CTxOut] aligned with fills. Adds a solver fee input."""
        solver = next(u for u in self.node.listunspent() if u["amount"] > 1)
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(o[0], 16), o[1])) for _, o, _ in fills]
        tx.vin.append(CTxIn(COutPoint(int(solver["txid"], 16), solver["vout"])))
        tx.vout = list(payouts)
        tx.vout.append(CTxOut(int(solver["amount"] * 10**8) - fee, CScript(self.spk())))
        tx.vout.append(CTxOut(0, marker_script(self.category, price)))
        signed = FromHex(CTransaction(), self.node.signrawtransactionwithwallet(ToHex(tx))["hex"])
        for i, (order, _, _) in enumerate(fills):
            signed.wit.vtxinwit[i].scriptWitness.stack = order.fill_witness(price)
        signed.rehash()
        return signed

    def check(self, tx, allowed, reason=None):
        result = self.node.testmempoolaccept([ToHex(tx)])[0]
        assert_equal(result["allowed"], allowed)
        if reason:
            assert reason in result["reject-reason"], result

    def run_test(self):
        self.node = self.nodes[0]
        self.addr = self.node.getnewaddress()
        self.node.generatetoaddress(110, self.addr)
        self.category = self.node.tokengenesis(self.node.getnewaddress(), 1000)["category"]
        self.node.generatetoaddress(1, self.addr)
        token_utxo = self.node.listtokens()["outputs"][0]

        keys = [generate_privkey() for _ in range(4)]
        owners = [self.spk() for _ in range(4)]
        sell1 = Order("sell", self.category, 10, 100, xonly(keys[0]), owners[0])
        sell2 = Order("sell", self.category, 5, 130, xonly(keys[1]), owners[1])
        buy1 = Order("buy", self.category, 10, 120, xonly(keys[2]), owners[2])
        buy2 = Order("buy", self.category, 5, 90, xonly(keys[3]), owners[3])

        self.log.info("Traders post sealed orders (parameters hidden in Taproot leaves)")
        sells_txid = self.post_orders([
            CTxOut(DUST, CScript(encode_token_prefix(self.category, amount=10) + bytes(sell1.script_pubkey))),
            CTxOut(DUST, CScript(encode_token_prefix(self.category, amount=5) + bytes(sell2.script_pubkey))),
            CTxOut(DUST, CScript(encode_token_prefix(self.category, amount=985) + self.spk())),
        ], token_input=token_utxo)
        buys_txid = self.post_orders([CTxOut(BUY_FUNDS, buy1.script_pubkey), CTxOut(BUY_FUNDS, buy2.script_pubkey)])
        outpoints = {sell1: (sells_txid, 1), sell2: (sells_txid, 2), buy1: (buys_txid, 1), buy2: (buys_txid, 2)}

        price, sells, buys = clearing_price([sell1, sell2, buy1, buy2])
        assert_equal((price, sells, buys), (110, [sell1], [buy1]))
        fills = [(sell1, outpoints[sell1], DUST), (buy1, outpoints[buy1], BUY_FUNDS)]

        def payouts(p_seller, p_buyer):
            return [CTxOut(10 * p_seller + DUST, CScript(owners[0])),
                    CTxOut(BUY_FUNDS - 10 * p_buyer, CScript(encode_token_prefix(self.category, amount=10) + owners[2]))]

        self.log.info("A price outside a matched order's limit is rejected")
        self.check(self.settlement(fills, 95, payouts(95, 95)), False, "OP_VERIFY")
        self.check(self.settlement(fills, 125, payouts(125, 125)), False, "OP_VERIFY")

        self.log.info("Price discrimination (seller paid below the marker price) is rejected")
        self.check(self.settlement(fills, 110, payouts(105, 110)), False, "NUMEQUALVERIFY")

        self.log.info("Settlement at the uniform clearing price is accepted")
        tx = self.settlement(fills, 110, payouts(110, 110))
        self.check(tx, True)
        self.node.sendrawtransaction(ToHex(tx))
        self.node.generatetoaddress(1, self.addr)
        txid = tx.rehash()
        assert_equal(self.node.gettxout(txid, 0)["value"], Decimal(10 * 110 + DUST) / 10**8)
        buyer_out = self.node.decoderawtransaction(ToHex(tx))["vout"][1]
        assert_equal(buyer_out["tokenData"]["amount"], "10")

        self.log.info("Orders that do not cross (sell at 130, buy at 90) cannot be filled")
        self.check(self.settlement([(sell2, outpoints[sell2], DUST), (buy2, outpoints[buy2], BUY_FUNDS)], 110,
                                   [CTxOut(5 * 110 + DUST, CScript(owners[1])),
                                    CTxOut(BUY_FUNDS - 5 * 110, CScript(encode_token_prefix(self.category, amount=5) + owners[3]))]),
                   False, "OP_VERIFY")

        self.log.info("Owners cancel unfilled orders")
        for order, key, owner in ((sell2, keys[1], owners[1]), (buy2, keys[3], owners[3])):
            spent = CTxOut(DUST if order.side == "sell" else BUY_FUNDS,
                           CScript((encode_token_prefix(self.category, amount=5) if order.side == "sell" else b"") + bytes(order.script_pubkey)))
            cancel = order.cancel(outpoints[order], spent, key, owner, 5_000)
            self.check(cancel, True)
            self.node.sendrawtransaction(ToHex(cancel))
        self.node.generatetoaddress(1, self.addr)


if __name__ == '__main__':
    BatchAuctionTest().main()
