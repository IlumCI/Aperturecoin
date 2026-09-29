#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end Stratum V2 mining through sv2-tp and aperture-sv2-miner.

The binaries are built out of tree by contrib/sv2-tp/build.sh. The test is
skipped unless both paths are given:

    SV2TP=<path to sv2-tp> SV2MINER=<path to aperture-sv2-miner> \\
        test/functional/feature_sv2.py

Checks:
- sv2-tp connects to the node over JSON-RPC and serves templates
- the miner completes the Noise handshake, mines with its own payout and the
  blocks are accepted
- every coinbase carries the mandatory development fund output
"""

import os
import re
import subprocess
import time

from test_framework.blocktools import DEVFUND_REGTEST_SCRIPT, devfund_amount
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, p2p_port, rpc_port

BLOCKS = 3


class Sv2Test(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-devfundendheight=1000"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.sv2tp = os.getenv("SV2TP")
        self.sv2miner = os.getenv("SV2MINER")
        if not (self.sv2tp and self.sv2miner and os.path.exists(self.sv2tp) and os.path.exists(self.sv2miner)):
            raise SkipTest("SV2TP and SV2MINER not set (build with contrib/sv2-tp/build.sh)")

    def run_test(self):
        node = self.nodes[0]
        node.generatetoaddress(1, node.getnewaddress())
        start_height = node.getblockcount()

        tp_datadir = os.path.join(self.options.tmpdir, "sv2-tp")
        os.makedirs(os.path.join(tp_datadir, "regtest"))
        sv2_port = p2p_port(5)
        cookie = os.path.join(node.datadir, "regtest", ".cookie")
        tp_log = os.path.join(tp_datadir, "tp.log")

        self.log.info("Start sv2-tp with the JSON-RPC backend")
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

            self.log.info("Mine %d blocks with aperture-sv2-miner over Stratum V2", BLOCKS)
            payout = node.getaddressinfo(node.getnewaddress())["scriptPubKey"]
            out = subprocess.run([self.sv2miner, f"-connect=127.0.0.1:{sv2_port}", f"-authority={authority}",
                                  f"-payout={payout}", "-dim=32", "-threads=2", f"-blocks={BLOCKS}"],
                                 capture_output=True, text=True, timeout=300)
            self.log.debug(out.stdout)
            assert_equal(out.returncode, 0)
            self.wait_until(lambda: node.getblockcount() == start_height + BLOCKS)

            self.log.info("Coinbases pay the miner's payout and the development fund")
            for height in range(start_height + 1, start_height + BLOCKS + 1):
                coinbase = node.getblock(node.getblockhash(height), 2)["tx"][0]
                scripts = [o["scriptPubKey"]["hex"] for o in coinbase["vout"]]
                assert_equal(scripts[0], payout)
                fund = [o for o in coinbase["vout"] if o["scriptPubKey"]["hex"] == DEVFUND_REGTEST_SCRIPT.hex()]
                assert_equal(len(fund), 1)
                assert_equal(int(round(fund[0]["value"] * 10**8)), devfund_amount(height))
        finally:
            tp.terminate()
            tp.wait(timeout=30)


if __name__ == '__main__':
    Sv2Test().main()
