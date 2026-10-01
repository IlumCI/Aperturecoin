#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""ApertureMatMul v2 panels trimmed from the in-memory block index.

Once a block index entry is written to the block index database, its r x r
activation panel is dropped from memory and read back on demand. Headers
served after a flush and after a restart must be byte-identical (same hash,
same panel), and a fresh node must sync from them.
"""

from io import BytesIO

from test_framework.messages import CBlockHeader
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

BLOCKS = 30


class PowV2IndexTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-powv2height=1"]] * 2

    def setup_network(self):
        self.setup_nodes()  # node1 connects later, after node0's restart

    def headers(self, node, hashes):
        out = {}
        for h in hashes:
            raw = node.getblockheader(h, False)
            hdr = CBlockHeader()
            hdr.deserialize(BytesIO(bytes.fromhex(raw)))
            hdr.rehash()
            assert_equal(hdr.hash, h)
            out[h] = (raw, node.getblockheader(h)["powv2"])
        return out

    def run_test(self):
        n0, n1 = self.nodes
        hashes = n0.generatetoaddress(BLOCKS, n0.getnewaddress())
        before = self.headers(n0, hashes)
        assert all(v[1]["panel_bytes"] > 0 for v in before.values())

        self.log.info("After a flush, headers are served from trimmed entries unchanged")
        n0.gettxoutsetinfo()  # forces a flush of the block index
        assert_equal(self.headers(n0, hashes), before)

        self.log.info("After a restart (entries load trimmed), headers are unchanged")
        self.restart_node(0)
        assert_equal(self.headers(n0, hashes), before)

        self.log.info("A fresh node syncs headers and blocks from the trimmed index")
        self.connect_nodes(1, 0)
        self.sync_blocks()
        assert_equal(n1.getbestblockhash(), hashes[-1])
        assert_equal(self.headers(n1, hashes), before)

        self.log.info("Entries rewritten after trimming keep their panel (invalidate/reconsider)")
        n0.invalidateblock(hashes[10])
        n0.gettxoutsetinfo()
        n0.reconsiderblock(hashes[10])
        n0.gettxoutsetinfo()
        assert_equal(n0.getbestblockhash(), hashes[-1])
        self.restart_node(0)
        assert_equal(self.headers(n0, hashes), before)


if __name__ == "__main__":
    PowV2IndexTest().main()
