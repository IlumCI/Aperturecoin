#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Consensus minimum fee per embedding request token (nMinRequestFeePerToken).

- estimaterequestfee quotes tokens x fee per token;
- the wallet pays at least the minimum, even when it exceeds the size-based fee;
- the mempool refuses an underpaying request (bad-embed-request-fee);
- a block with an underpaying request is invalid.
"""

from decimal import Decimal

from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut, ToHex
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than_or_equal

FEE_PER_TOKEN = 5000  # sat; well above the size-based fee of a small request


class RequestFeeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # node1 has no minimum, so it can build a block that node0 must reject.
        self.extra_args = [["-powv2height=1", f"-powv2tokenfee={FEE_PER_TOKEN}"], ["-powv2height=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def request_tx(self, node, text, fee):
        utxo = node.listunspent()[0]
        ids = b"".join(c.to_bytes(3, "little") for c in text.encode())
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]))]
        change = node.getaddressinfo(node.getnewaddress())["scriptPubKey"]
        tx.vout = [CTxOut(0, CScript([OP_RETURN, b"APER", ids])),
                   CTxOut(int(utxo["amount"] * COIN) - fee, bytes.fromhex(change))]
        return node.signrawtransactionwithwallet(ToHex(tx))["hex"]

    def run_test(self):
        n0, n1 = self.nodes
        addr = n0.getnewaddress()
        n1.generatetoaddress(10, n1.getnewaddress())
        n0.generatetoaddress(110, addr)
        self.sync_all()

        self.log.info("estimaterequestfee quotes tokens x fee per token")
        quote = n0.estimaterequestfee("hello world")
        assert_equal(quote["tokens"], 12)
        assert_equal(quote["fee_per_token"], Decimal(FEE_PER_TOKEN) / COIN)
        assert_equal(quote["min_fee"], Decimal(12 * FEE_PER_TOKEN) / COIN)

        self.log.info("The wallet pays at least the minimum")
        sent = n0.sendembeddingrequest("hello world")
        assert_greater_than_or_equal(sent["fee"], quote["min_fee"])
        assert sent["txid"] in n0.getrawmempool()
        self.sync_mempools()
        block = n0.generatetoaddress(1, addr)[0]
        assert_equal(len(n0.getblockembeddings(block)), 1)
        self.sync_all()

        self.log.info("The mempool refuses an underpaying request")
        low = self.request_tx(n0, "hello world", 12 * FEE_PER_TOKEN - 1)
        assert_equal(n0.testmempoolaccept([low])[0]["reject-reason"], "bad-embed-request-fee")
        exact = self.request_tx(n0, "hello world", 12 * FEE_PER_TOKEN)
        assert n0.testmempoolaccept([exact])[0]["allowed"]

        self.log.info("A block with an underpaying request is invalid")
        self.disconnect_nodes(0, 1)
        n1.sendrawtransaction(self.request_tx(n1, "cheap", 2000))
        bad = n1.generatetoaddress(1, n1.getnewaddress())[0]
        assert_equal(len(n1.getblockembeddings(bad)), 1)
        assert_equal(n0.submitblock(n1.getblock(bad, 0)), "bad-embed-request-fee")
        assert n0.getbestblockhash() != bad


if __name__ == "__main__":
    RequestFeeTest().main()
