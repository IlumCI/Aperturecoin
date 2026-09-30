#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Pooled mining through the ApertureCoin fork of the SRI stack (doc/stratum-v2.md).

    SV2TP=<sv2-tp> SRI_POOL=<pool_sv2> SRI_MINING_DEVICE=<mining_device> \\
        test/functional/feature_sri_pool.py

Topology: apertured <- RPC/IPC - sv2-tp <- Template Distribution - SRI pool
<- Mining protocol - SRI mining device. The pool and the device validate
shares with ApertureMatMul (dimension 32 on regtest), not SHA256d. Checks:
- the pool accepts an ApertureCoin (rsci1) payout address in addr();
- blocks found by the device through the pool are accepted by the node;
- their coinbase pays the pool's script and keeps the template's outputs;
- every share the device found and the pool accepted is the ApertureMatMul
  hash of the accepted block's header (a SHA256d validator would reject it).
"""

import hashlib
import os
import re
import subprocess
import time

from test_framework.aperture_matmulpow import getPoWHash
from test_framework.blocktools import DEVFUND_REGTEST_SCRIPT, devfund_amount
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, assert_greater_than, p2p_port, rpc_port

BLOCKS = 3
# SRI example authority keys (regtest only).
POOL_PUB = "9auqWEzQDVyd2oe1JVGFLMLHZtCo2FFqZwtKA5gd9xbuEu7PH72"
POOL_SEC = "mkDLTBBRxdBv998612qipDYoTK3YUrqLe8uWw7gu3iXbSrn2n"


class SriPoolTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Enforce the development fund so the pool must carry the template's fund output.
        self.extra_args = [["-devfundendheight=1000"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.bins = {k: os.getenv(k) for k in ("SV2TP", "SRI_POOL", "SRI_MINING_DEVICE")}
        if not all(v and os.path.exists(v) for v in self.bins.values()):
            raise SkipTest("SV2TP, SRI_POOL and SRI_MINING_DEVICE not set (see doc/stratum-v2.md)")

    def wait_for_log(self, path, pattern, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with open(path, encoding="utf8", errors="replace") as f:
                m = re.search(pattern, f.read())
            if m:
                return m
            time.sleep(0.1)
        with open(path, encoding="utf8", errors="replace") as f:
            self.log.error(f.read()[-4000:])
        raise AssertionError(f"{pattern!r} not found in {path}")

    def run_test(self):
        node = self.nodes[0]
        node.generatetoaddress(1, node.getnewaddress())
        start_height = node.getblockcount()
        pool_addr = node.getnewaddress("", "bech32")
        assert pool_addr.startswith("rsci1")
        pool_spk = node.getaddressinfo(pool_addr)["scriptPubKey"]
        procs = []
        try:
            self.log.info("Start sv2-tp")
            tp_dir = os.path.join(self.options.tmpdir, "sv2-tp")
            os.makedirs(os.path.join(tp_dir, "regtest"))
            tp_port = p2p_port(5)
            tp_log = os.path.join(tp_dir, "tp.log")
            cookie = os.path.join(node.datadir, "regtest", ".cookie")
            procs.append(subprocess.Popen(
                [self.bins["SV2TP"], "-regtest", f"-datadir={tp_dir}", f"-rpcport={rpc_port(0)}",
                 f"-rpccookiefile={cookie}", f"-sv2port={tp_port}", "-debug=sv2", "-printtoconsole"],
                stdout=open(tp_log, "w", encoding="utf8"), stderr=subprocess.STDOUT))
            authority = self.wait_for_log(tp_log, r"authority key: (\w+)").group(1)

            self.log.info("Start the SRI pool (ApertureMatMul dim 32, rsci1 payout)")
            pool_dir = os.path.join(self.options.tmpdir, "pool")
            os.makedirs(pool_dir)
            pool_port = p2p_port(6)
            cfg = os.path.join(pool_dir, "pool.toml")
            with open(cfg, "w", encoding="utf8") as f:
                f.write(f"""authority_public_key = "{POOL_PUB}"
authority_secret_key = "{POOL_SEC}"
cert_validity_sec = 3600
listen_address = "127.0.0.1:{pool_port}"
coinbase_reward_script = "addr({pool_addr})"
server_id = 1
pool_signature = "ApertureCoin SRI pool test"
shares_per_minute = 60.0
share_batch_size = 1
aperture_pow_dim = 32

[template_provider_type.Sv2Tp]
address = "127.0.0.1:{tp_port}"
public_key = "{authority}"
""")
            pool_log = os.path.join(pool_dir, "pool.log")
            procs.append(subprocess.Popen([self.bins["SRI_POOL"], "-c", cfg],
                                          stdout=open(pool_log, "w", encoding="utf8"), stderr=subprocess.STDOUT))
            self.wait_for_log(pool_log, r"ApertureMatMul proof of work, dimension 32")
            self.wait_for_log(pool_log, rf"127\.0\.0\.1:{pool_port}|[Ll]istening", timeout=60)

            self.log.info("Mine through the pool with the SRI mining device")
            dev_log = os.path.join(pool_dir, "device.log")
            env = dict(os.environ, APERTURE_POW_DIM="32")
            procs.append(subprocess.Popen(
                [self.bins["SRI_MINING_DEVICE"], "--address-pool", f"127.0.0.1:{pool_port}",
                 "--pubkey-pool", POOL_PUB, "--id-user", "sri/donate/aperture-test", "--cores", "2"],
                stdout=open(dev_log, "w", encoding="utf8"), stderr=subprocess.STDOUT, env=env))
            self.wait_until(lambda: node.getblockcount() >= start_height + BLOCKS, timeout=600)

            self.log.info("Pooled blocks are valid and pay the pool")
            with open(dev_log, encoding="utf8", errors="replace") as f:
                share_hashes = [bytes(int(x) for x in m.group(1).split(","))
                                for m in re.finditer(r"Found share with nonce: \d+, .*?with hash: \[([\d, ]+)\]", f.read())]
            assert_greater_than(len(share_hashes), 0)
            for height in range(start_height + 1, start_height + BLOCKS + 1):
                block_hash = node.getblockhash(height)
                block = node.getblock(block_hash, 2)
                coinbase = block["tx"][0]
                spks = [o["scriptPubKey"]["hex"] for o in coinbase["vout"]]
                assert pool_spk in spks, spks
                # Besides the pool payout, the template's own outputs (witness
                # commitment, dev fund) are carried over.
                devfund = [o for o in coinbase["vout"] if o["scriptPubKey"]["hex"] == DEVFUND_REGTEST_SCRIPT.hex()]
                assert_equal(len(devfund), 1)
                assert int(round(devfund[0]["value"] * 100_000_000)) >= devfund_amount(height)
                header = bytes.fromhex(node.getblockheader(block_hash, False))
                pow_hash = getPoWHash(header)
                assert pow_hash in share_hashes, f"block {height}: ApertureMatMul hash not among the device's shares"
                # Share targets need 16 leading zero bits; SHA256d of the header does not meet them.
                assert pow_hash[30:] == b"\0\0"
                assert_equal(hashlib.sha256(hashlib.sha256(header).digest()).digest()[::-1].hex(), block_hash)
            self.log.info("%d shares accepted, all ApertureMatMul", len(share_hashes))
        finally:
            for p in reversed(procs):
                p.terminate()
            for p in procs:
                try:
                    p.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    p.kill()


if __name__ == '__main__':
    SriPoolTest().main()
