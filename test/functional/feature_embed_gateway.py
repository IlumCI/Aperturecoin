#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""OpenAI-compatible embeddings gateway (contrib/aperture-embed-gateway).

- POST /v1/embeddings returns the protocol-model embedding at once (local,
  bit-exact) and settles the input as an on-chain request;
- once mined, GET /v1/requests/<outpoint> reports the on-chain result, which
  equals the local one and a second node's;
- aperture.wait holds the response until the request is mined;
- batch, token-id and base64 inputs; bad input is a 400 error.
"""

import base64
import json
import os
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, get_datadir_path, rpc_port

GATEWAY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "contrib", "aperture-embed-gateway", "gateway.py")


class EmbedGatewayTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-powv2height=1"]] * 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def http(self, method, path, body=None):
        req = urllib.request.Request(self.base + path, method=method,
                                     data=None if body is None else json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.status, json.load(r)
        except urllib.error.HTTPError as e:
            return e.code, json.load(e)

    def start_gateway(self):
        cookie = os.path.join(get_datadir_path(self.options.tmpdir, 0), self.chain, ".cookie")
        url = f"http://127.0.0.1:{rpc_port(0)}/wallet/{self.default_wallet_name}"
        self.gw = subprocess.Popen([sys.executable, GATEWAY, f"--rpcurl={url}", f"--rpccookiefile={cookie}", "--port=0"],
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        line = self.gw.stdout.readline()
        assert line.startswith("listening on "), line
        self.base = line.split()[-1]

    def run_test(self):
        node, other = self.nodes
        addr = node.getnewaddress()
        node.generatetoaddress(110, addr)
        self.sync_all()
        self.start_gateway()
        try:
            self.check(node, other, addr)
        finally:
            self.gw.terminate()
            self.gw.wait()

    def check(self, node, other, addr):
        self.log.info("GET /v1/models reports the protocol model")
        code, models = self.http("GET", "/v1/models")
        assert_equal(code, 200)
        model_id = node.embed("x")["model_id"]
        assert_equal(models["data"][0]["model_id"], model_id)
        assert_equal(models["data"][0]["pricing"]["currency"], "SCIENCE")

        self.log.info("POST /v1/embeddings: OpenAI shape, exact local result, settled on chain")
        code, res = self.http("POST", "/v1/embeddings", {"input": "hello world", "model": "aperture"})
        assert_equal(code, 200)
        assert_equal(res["object"], "list")
        item = res["data"][0]
        assert_equal(item["object"], "embedding")
        assert_equal(item["index"], 0)
        local = node.embed("hello world")
        assert_equal(item["aperture"]["int8"], local["embedding"])
        assert_equal(item["aperture"]["status"], "pending")
        assert_equal(res["usage"]["prompt_tokens"], local["tokens"])
        norm = sum(x * x for x in item["embedding"])
        assert abs(norm - 1.0) < 1e-9, norm
        outpoint = item["aperture"]["request"]
        assert outpoint["txid"] in node.getrawmempool()

        code, status = self.http("GET", f"/v1/requests/{outpoint['txid']}:{outpoint['vout']}")
        assert_equal((code, status["status"]), (200, "pending"))

        self.log.info("Once mined, the on-chain result equals the local one and a second node's")
        block = node.generatetoaddress(1, addr)[0]
        self.sync_all()
        code, status = self.http("GET", f"/v1/requests/{outpoint['txid']}:{outpoint['vout']}")
        assert_equal((code, status["status"], status["blockhash"]), (200, "confirmed", block))
        assert_equal(status["int8"], local["embedding"])
        assert_equal(status["embedding"], item["embedding"])
        served = [r for r in other.getblockembeddings(block) if r["txid"] == outpoint["txid"]]
        assert_equal(served[0]["embedding"], local["embedding"])
        assert all(r["valid"] for r in other.checkblockembeddings(block))

        self.log.info("aperture.wait holds the response until the request is mined")
        mined = []
        miner = threading.Timer(2.0, lambda: mined.append(node.generatetoaddress(1, addr)[0]))
        miner.start()
        code, res = self.http("POST", "/v1/embeddings", {"input": ["alpha", [104, 105]], "aperture": {"wait": 60}})
        miner.join()
        assert_equal(code, 200)
        assert_equal(len(res["data"]), 2)
        for d in res["data"]:
            assert_equal(d["aperture"]["status"], "confirmed")
            assert_equal(d["aperture"]["blockhash"], mined[0])
            assert_equal(d["aperture"]["matches_local"], True)
        assert_equal(res["data"][1]["aperture"]["int8"], node.embed("[104, 105]")["embedding"])

        self.log.info("settle=false computes locally without a transaction; base64 encoding")
        mempool = node.getrawmempool()
        code, res = self.http("POST", "/v1/embeddings", {"input": "local only", "encoding_format": "base64",
                                                         "aperture": {"settle": False}})
        assert_equal(code, 200)
        assert_equal(node.getrawmempool(), mempool)
        d = res["data"][0]
        assert_equal(d["aperture"]["status"], "local")
        raw = base64.b64decode(d["embedding"])
        vec = struct.unpack(f"<{len(raw) // 4}f", raw)
        assert_greater_than(len(vec), 0)
        assert abs(sum(x * x for x in vec) - 1.0) < 1e-4

        self.log.info("Bad input is a 400 error")
        for body in [{"input": 5}, {"input": []}, {"input": [[1, "a"]]}, {"input": "x", "encoding_format": "hex"}]:
            code, res = self.http("POST", "/v1/embeddings", body)
            assert_equal(code, 400)
            assert "error" in res
        code, res = self.http("POST", "/v1/embeddings", {"input": "y" * 100000, "aperture": {"settle": False}})
        assert_equal((code, res["error"]["type"]), (400, "node_error"))
        assert_equal(self.http("GET", "/v1/nothing")[0], 404)


if __name__ == "__main__":
    EmbedGatewayTest().main()
