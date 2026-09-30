#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Pooled ApertureMatMul v2 (proof of useful work) through the SRI fork (doc/stratum-v2.md).

    SV2TP=<sv2-tp> SRI_POOL=<pool_sv2> APERTURE_POUW_MINER=<aperture-pouw-miner> \\
        test/functional/feature_sri_pool_pouw.py

Topology: apertured <- sv2-tp (UsefulWorkTemplate 0x4150/0x01) <- SRI pool
(forward pass per template, SetUsefulWork 0x4150/0x03) <- aperture-pouw-miner
(forward pass, ticket search, shares with a 0x4150 ticket TLV). Checks:
- pooled blocks are v2, accepted, pay the pool and keep the dev-fund output;
- they serve the pending embedding requests, bit-identical to the node's model;
- each block's ticket is one the miner submitted and the pool accepted;
- a miner that submits panels other than the forward pass's activations is
  rejected by the pool (useful-work-panel-mismatch), although the ticket hashes
  would satisfy consensus.
"""

import os
import re
import subprocess
import time

from test_framework.blocktools import DEVFUND_REGTEST_SCRIPT, devfund_amount
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, assert_greater_than, p2p_port, rpc_port

ACTIVATION = 102
BLOCKS = 3
TEXTS = ["pooled useful work", "the final embeddings are the block"]
POOL_PUB = "9auqWEzQDVyd2oe1JVGFLMLHZtCo2FFqZwtKA5gd9xbuEu7PH72"
POOL_SEC = "mkDLTBBRxdBv998612qipDYoTK3YUrqLe8uWw7gu3iXbSrn2n"


class SriPoolPoUWTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[f"-powv2height={ACTIVATION}", "-devfundendheight=1000"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.bins = {k: os.getenv(k) for k in ("SV2TP", "SRI_POOL", "APERTURE_POUW_MINER")}
        if not all(v and os.path.exists(v) for v in self.bins.values()):
            raise SkipTest("SV2TP, SRI_POOL and APERTURE_POUW_MINER not set (see doc/stratum-v2.md)")

    def wait_for_log(self, path, pattern, timeout=60):
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

    def start_miner(self, pool_port, log, *extra):
        return subprocess.Popen(
            [self.bins["APERTURE_POUW_MINER"], "--pool", f"127.0.0.1:{pool_port}", "--pool-key", POOL_PUB,
             "--user", "sri/donate/pouw-test", "--tinymodel", "1", "--rank", "8", "--threads", "2", *extra],
            stdout=open(log, "w", encoding="utf8"), stderr=subprocess.STDOUT)

    def run_test(self):
        node = self.nodes[0]
        node.generatetoaddress(ACTIVATION + 1, node.getnewaddress())
        txids = [node.sendembeddingrequest(t)["txid"] for t in TEXTS]
        start_height = node.getblockcount()
        pool_addr = node.getnewaddress("", "bech32")
        pool_spk = node.getaddressinfo(pool_addr)["scriptPubKey"]
        procs = []
        try:
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

            self.log.info("Start the SRI pool with the regtest protocol model")
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
pool_signature = "ApertureCoin useful-work pool test"
shares_per_minute = 60.0
share_batch_size = 1
aperture_pow_dim = 32
aperture_tiny_model = 1

[template_provider_type.Sv2Tp]
address = "127.0.0.1:{tp_port}"
public_key = "{authority}"
""")
            pool_log = os.path.join(pool_dir, "pool.log")
            procs.append(subprocess.Popen([self.bins["SRI_POOL"], "-c", cfg],
                                          stdout=open(pool_log, "w", encoding="utf8"), stderr=subprocess.STDOUT))
            self.wait_for_log(pool_log, r"ApertureMatMul v2: protocol model f4cb7bf6")
            self.wait_for_log(pool_log, rf"Useful work for template \d+: {len(TEXTS)} embedding requests")

            self.log.info("A miner submitting panels that are not the forward pass is rejected by the pool")
            bad_log = os.path.join(pool_dir, "miner-corrupt.log")
            bad = self.start_miner(pool_port, bad_log, "--corrupt-panels")
            self.wait_for_log(bad_log, r"rejected seq=\d+ useful-work-panel-mismatch", timeout=120)
            bad.terminate()
            bad.wait(timeout=30)
            assert_equal(node.getblockcount(), start_height)
            with open(bad_log, encoding="utf8") as f:
                assert "accepted seq=" not in f.read()

            self.log.info("An honest miner mines %d v2 blocks through the pool", BLOCKS)
            miner_log = os.path.join(pool_dir, "miner.log")
            procs.append(self.start_miner(pool_port, miner_log))
            self.wait_until(lambda: node.getblockcount() >= start_height + BLOCKS, timeout=600)
            self.wait_for_log(pool_log, r"SubmitUsefulWorkSolution for template \d+ sent")

            with open(miner_log, encoding="utf8") as f:
                text = f.read()
            submitted = {m.group(2): m.group(1) for m in re.finditer(r"share job=\d+ nonce=\d+ ticket=(\S+) pow=(\w+)", text)}
            accepted = set(re.findall(r"accepted seq=\d+ job=\d+ pow=(\w+)", text))
            assert_greater_than(len(accepted), 0)
            self.log.info("miner: %d shares submitted, %d accepted", len(submitted), len(accepted))

            served = {}
            for height in range(start_height + 1, start_height + BLOCKS + 1):
                block_hash = node.getblockhash(height)
                hdr = node.getblockheader(block_hash)
                assert hdr["version"] & 0x100, hdr
                v2 = hdr["powv2"]
                assert_equal(v2["panel_bytes"], 8 * 8)
                pow_raw = bytes.fromhex(v2["pow_hash"])[::-1].hex()
                assert pow_raw in accepted, f"block {height}: ticket not among the pool-accepted shares"
                assert_equal(submitted[pow_raw], f"{v2['op']},{v2['tile_i']},{v2['tile_j']},{v2['span_s']}")
                coinbase = node.getblock(block_hash, 2)["tx"][0]
                spks = [o["scriptPubKey"]["hex"] for o in coinbase["vout"]]
                assert pool_spk in spks, spks
                devfund = [o for o in coinbase["vout"] if o["scriptPubKey"]["hex"] == DEVFUND_REGTEST_SCRIPT.hex()]
                assert_equal(len(devfund), 1)
                assert int(round(devfund[0]["value"] * 100_000_000)) >= devfund_amount(height)
                for e in node.getblockembeddings(block_hash):
                    served[e["txid"]] = e["embedding"]

            self.log.info("The pooled blocks served the embedding requests exactly")
            assert_equal(sorted(served), sorted(txids))
            for text_in, txid in zip(TEXTS, txids):
                assert_equal(served[txid], node.embed(text_in)["embedding"])
        finally:
            for p in reversed(procs):
                p.terminate()
            for p in procs:
                try:
                    p.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    p.kill()


if __name__ == '__main__':
    SriPoolPoUWTest().main()
