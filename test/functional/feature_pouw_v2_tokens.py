#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""ApertureMatMul v2 per-block token budget (Consensus::Params::nMaxEmbedTokens).

- a transaction whose requests exceed the budget is refused by the mempool;
- the miner fills blocks up to the budget and defers the rest;
- a block over the budget is invalid (bad-embed-tokens).
"""

from decimal import Decimal

from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut, ToHex
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

BUDGET = 20


class PowV2TokenBudgetTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # node1 has no budget, so it can build a block that node0 must reject.
        self.extra_args = [["-powv2height=1", f"-powv2maxtokens={BUDGET}"], ["-powv2height=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        n0, n1 = self.nodes
        addr = n0.getnewaddress()
        n1.generatetoaddress(10, n1.getnewaddress())
        n0.generatetoaddress(110, addr)
        self.sync_all()

        self.log.info("A request over the budget is refused by the wallet and by the mempool")
        assert_raises_rpc_error(-8, "bad-embed-tokens", n0.sendembeddingrequest, "x" * BUDGET)
        utxo = n0.listunspent()[0]
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]))]
        ids = b"".join(c.to_bytes(3, "little") for c in b"x" * BUDGET)
        change = n0.getaddressinfo(n0.getnewaddress())["scriptPubKey"]
        tx.vout = [CTxOut(0, CScript([OP_RETURN, b"APER", ids])),
                   CTxOut(int((utxo["amount"] - Decimal("0.001")) * COIN), bytes.fromhex(change))]
        signed = n0.signrawtransactionwithwallet(ToHex(tx))["hex"]
        assert_equal(n0.testmempoolaccept([signed])[0]["reject-reason"], "bad-embed-tokens")
        tx.vout[0] = CTxOut(0, CScript([OP_RETURN, b"APER", ids[:3 * (BUDGET - 1)]]))
        assert n0.testmempoolaccept([n0.signrawtransactionwithwallet(ToHex(tx))["hex"]])[0]["allowed"]

        self.log.info("The miner fills each block up to the budget and defers the rest")
        a = n0.sendembeddingrequest("a" * 12)  # 13 tokens with EOS
        b = n0.sendembeddingrequest("b" * 12)
        self.sync_mempools()
        first = n0.generatetoaddress(1, addr)[0]
        served = n0.getblockembeddings(first)
        assert_equal(len(served), 1)
        second = n0.generatetoaddress(1, addr)[0]
        assert_equal(len(n0.getblockembeddings(second)), 1)
        assert_equal(sorted(r["txid"] for r in n0.getblockembeddings(first) + n0.getblockembeddings(second)),
                     sorted([a["txid"], b["txid"]]))
        self.sync_all()

        self.log.info("A block over the budget is invalid")
        self.disconnect_nodes(0, 1)
        c = n1.sendembeddingrequest("c" * 12)
        d = n1.sendembeddingrequest("d" * 12)
        block = n1.generatetoaddress(1, n1.getnewaddress())[0]
        assert_equal(sorted(r["txid"] for r in n1.getblockembeddings(block)), sorted([c["txid"], d["txid"]]))
        assert_equal(n0.submitblock(n1.getblock(block, 0)), "bad-embed-tokens")
        assert n0.getbestblockhash() != block


if __name__ == "__main__":
    PowV2TokenBudgetTest().main()
