#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""getusefulshare: usefulness accounting of ApertureMatMul v2 blocks.

useful_share = served_tokens * macs_per_token / (pass_macs + ticket_macs), where a
block's ticket work is its expected tickets (chainwork increment) times r^3.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_approx, assert_equal, assert_raises_rpc_error

RANK = 8  # regtest


class UsefulShareTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-powv2height=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def proof(self, h):
        node = self.nodes[0]
        hdr = node.getblockheader(h)
        prev = node.getblockheader(hdr["previousblockhash"])
        return int(hdr["chainwork"], 16) - int(prev["chainwork"], 16)

    def run_test(self):
        node = self.nodes[0]
        addr = node.getnewaddress()
        node.generatetoaddress(110, addr)

        self.log.info("Served requests count as useful work, empty blocks do not")
        node.sendembeddingrequest("hello world")    # 11 + EOS
        node.sendembeddingrequest("useful work")    # 11 + EOS
        served = node.generatetoaddress(1, addr)[0]
        empty = node.generatetoaddress(1, addr)[0]
        res = node.getusefulshare(2, None, True)
        assert_equal((res["blocks"], res["requests"], res["served_tokens"], res["empty_blocks"]), (2, 2, 24, 1))
        mpt = res["macs_per_token"]
        assert_equal(res["useful_macs"], 24 * mpt)
        assert_equal(res["pass_macs"], 24 * mpt + 1 * mpt)
        tickets = (self.proof(served) + self.proof(empty)) * RANK ** 3
        assert_equal(res["ticket_macs"], tickets)
        assert_approx(res["useful_share"], 24 * mpt / (25 * mpt + tickets), vspan=1e-9)
        per = {b["hash"]: b for b in res["per_block"]}
        assert_equal(per[empty]["useful_share"], 0)
        assert_approx(per[served]["useful_share"], 24 * mpt / (24 * mpt + self.proof(served) * RANK ** 3), vspan=1e-9)

        self.log.info("Window ending at a given block; v1 blocks end the window")
        res = node.getusefulshare(1, served)
        assert_equal((res["blocks"], res["served_tokens"]), (1, 24))
        assert_equal(node.getusefulshare(1000)["blocks"], node.getblockcount())
        assert_raises_rpc_error(-8, "nblocks must be positive", node.getusefulshare, 0)
        assert_raises_rpc_error(-5, "Block not found", node.getusefulshare, 1, "00" * 32)


if __name__ == "__main__":
    UsefulShareTest().main()
