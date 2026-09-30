#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""External ApertureMatMul v2 mining over Stratum V2 (doc/stratum-v2.md).

    SV2TP=<path to sv2-tp> SV2MINER=<path to aperture-sv2-miner> \\
        test/functional/feature_sv2_pouw.py

With v2 active, sv2-tp announces every template's embedding requests with the
ApertureCoin extension message (0x4150/0x01). aperture-sv2-miner runs the
protocol model over them itself, searches tickets over its own activations
and submits SubmitUsefulWorkSolution (0x4150/0x02); sv2-tp rebuilds the block
with the v2 header extension. Checks:
- blocks mined externally carry v2 headers and are accepted;
- pending embedding requests are served by those blocks, bit-identical to the
  node's own model;
- the coinbase pays the external miner's payout and carries the results.
"""

import os
import re
import subprocess
import time

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, p2p_port, rpc_port

ACTIVATION = 102
BLOCKS = 3
TEXTS = ["stratum v2 useful work", "external miner, own activations"]


class Sv2PoUWTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[f"-powv2height={ACTIVATION}"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.sv2tp = os.getenv("SV2TP")
        self.sv2miner = os.getenv("SV2MINER")
        if not (self.sv2tp and self.sv2miner and os.path.exists(self.sv2tp) and os.path.exists(self.sv2miner)):
            raise SkipTest("SV2TP and SV2MINER not set (build with contrib/sv2-tp/build.sh)")

    def run_test(self):
        node = self.nodes[0]
        node.generatetoaddress(ACTIVATION + 1, node.getnewaddress())
        start_height = node.getblockcount()
        txids = [node.sendembeddingrequest(t)["txid"] for t in TEXTS]

        self.log.info("getblocktemplate serves the v2 work")
        gbt = node.getblocktemplate({"rules": ["segwit", "mweb"]})
        assert gbt["version"] & 0x100
        assert_equal(len(gbt["powv2"]["requests"]), len(TEXTS))
        assert_equal(len(gbt["powv2"]["results"]), len(TEXTS))

        tp_datadir = os.path.join(self.options.tmpdir, "sv2-tp")
        os.makedirs(os.path.join(tp_datadir, "regtest"))
        sv2_port = p2p_port(5)
        cookie = os.path.join(node.datadir, "regtest", ".cookie")
        tp_log = os.path.join(tp_datadir, "tp.log")
        with open(tp_log, "w", encoding="utf8") as log:
            tp = subprocess.Popen([self.sv2tp, "-regtest", f"-datadir={tp_datadir}", f"-rpcport={rpc_port(0)}",
                                   f"-rpccookiefile={cookie}", f"-sv2port={sv2_port}", "-debug=sv2", "-printtoconsole"],
                                  stdout=log, stderr=subprocess.STDOUT)
        try:
            authority = None
            for _ in range(100):
                with open(tp_log, encoding="utf8") as f:
                    m = re.search(r"authority key: (\w+)", f.read())
                if m:
                    authority = m.group(1)
                    break
                time.sleep(0.1)
            assert authority, "sv2-tp did not start"

            self.log.info("aperture-sv2-miner mines %d v2 blocks over Stratum V2 with its own forward passes", BLOCKS)
            payout = node.getaddressinfo(node.getnewaddress())["scriptPubKey"]
            out = subprocess.run([self.sv2miner, f"-connect=127.0.0.1:{sv2_port}", f"-authority={authority}",
                                  f"-payout={payout}", "-dim=32", "-tinymodel", "-threads=2", f"-blocks={BLOCKS}"],
                                 capture_output=True, text=True, timeout=300)
            self.log.debug(out.stdout)
            assert_equal(out.returncode, 0)
            assert "found useful-work block" in out.stdout, out.stdout
            assert "2 embedding requests" in out.stdout, out.stdout
            self.wait_until(lambda: node.getblockcount() == start_height + BLOCKS)

            self.log.info("Blocks are v2, pay the external miner and serve the requests exactly")
            served = {}
            for height in range(start_height + 1, start_height + BLOCKS + 1):
                bh = node.getblockhash(height)
                hdr = node.getblockheader(bh)
                assert hdr["version"] & 0x100
                assert_equal(hdr["powv2"]["panel_bytes"], 8 * 8)
                coinbase = node.getblock(bh, 2)["tx"][0]
                assert_equal(coinbase["vout"][0]["scriptPubKey"]["hex"], payout)
                for e in node.getblockembeddings(bh):
                    served[e["txid"]] = e["embedding"]
            assert_equal(sorted(served), sorted(txids))
            for text, txid in zip(TEXTS, txids):
                assert_equal(served[txid], node.embed(text)["embedding"])
        finally:
            tp.terminate()
            tp.wait(timeout=30)


if __name__ == '__main__':
    Sv2PoUWTest().main()
