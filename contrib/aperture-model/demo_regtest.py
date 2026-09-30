#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end demo: a regtest node mines useful work with a real model.

Usage: demo_regtest.py <src_dir> <hf_dir> <model.apm>

Starts a throwaway regtest node (-powv2height=102, -protocolmodel=<model.apm>),
pays for embedding requests tokenized with the model's own tokenizer, mines
them (the miner runs the forward pass and searches PoW tickets over its real
activations), checks every served embedding against the Python integer
reference, then runs semantic search over what the miner produced.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np
from tokenizers import Tokenizer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aperture_model.qwen3 import IntModel  # noqa: E402

DOCS = [
    "Paris is the capital and largest city of France.",
    "Tensor cores accelerate dense matrix multiply-accumulate operations.",
    "Vaccines train the immune system to recognize a pathogen.",
    "Miners compete to find a nonce whose block hash is below a target.",
    "Rinse the rice, then simmer it covered for eighteen minutes.",
]
QUERIES = [
    ("What is the capital of France?", 0),
    ("How does proof of work mining function?", 3),
    ("how to cook rice", 4),
    ("GPU hardware for fast linear algebra", 1),
]
QUERY_PREFIX = "Instruct: Given a web search query, retrieve relevant passages that answer the query\nQuery:"


def main():
    src, hf_dir, apm_path = sys.argv[1:4]
    tok = Tokenizer.from_file(os.path.join(hf_dir, "tokenizer.json"))
    datadir = tempfile.mkdtemp(prefix="aperture_demo_")
    cli = [os.path.join(src, "aperture-cli"), f"-datadir={datadir}", "-regtest", "-rpcclienttimeout=0"]

    def rpc(*args):
        argv = cli + [a if isinstance(a, str) else json.dumps(a) for a in args]
        out = subprocess.run(argv, check=True, capture_output=True, text=True).stdout.strip()
        try:
            return json.loads(out)
        except json.JSONDecodeError:
            return out

    node = subprocess.Popen([os.path.join(src, "apertured"), f"-datadir={datadir}", "-regtest",
                             "-powv2height=102", f"-protocolmodel={apm_path}", "-fallbackfee=0.0001"],
                            stdout=subprocess.DEVNULL)
    try:
        for _ in range(300):
            try:
                rpc("getblockcount")
                break
            except subprocess.CalledProcessError:
                time.sleep(1)
        rpc("createwallet", "demo")
        addr = rpc("getnewaddress")
        rpc("generatetoaddress", 101, addr)
        info = rpc("embed", tok.encode("hello").ids)
        print(f"protocol model {info['model_id']}")

        reqs = []
        for d in DOCS:
            r = rpc("sendembeddingrequest", json.dumps(tok.encode(d).ids))
            reqs.append(r["txid"])
            print(f"request {r['txid'][:16]}  {r['tokens']:3d} tokens  fee {r['fee']}  {d}")
        t0 = time.time()
        block = rpc("generatetoaddress", 1, addr)[0]
        print(f"mined {block[:16]} in {time.time() - t0:.1f}s (forward pass + ticket search + validation)")
        hdr = rpc("getblockheader", block)
        print("ticket", {k: hdr["powv2"][k] for k in ("op", "tile_i", "tile_j", "span_s", "pow_hash")})

        served = {e["txid"]: bytes.fromhex(e["embedding"]) for e in rpc("getblockembeddings", block)}
        ref = IntModel(apm_path)
        for d, txid in zip(DOCS, reqs):
            mine = np.frombuffer(served[txid], dtype=np.int8)
            want = ref.embed_ids(tok.encode(d).ids + [ref.c["eos_token_id"]])
            assert np.array_equal(mine, want), "node embedding differs from the Python reference"
        print(f"all {len(DOCS)} served embeddings equal the Python integer reference bit for bit")

        correct = 0
        for q, expect in QUERIES:
            top = rpc("searchembeddings", json.dumps(tok.encode(QUERY_PREFIX + q).ids), 10, 1)[0]
            hit = reqs.index(top["txid"])
            correct += hit == expect
            print(f"search {q!r:45s} -> {DOCS[hit]!r} (score {top['score']:.3f})")
        print(f"semantic search over mined embeddings: {correct}/{len(QUERIES)} correct")
    finally:
        try:
            rpc("stop")
        except Exception:
            node.terminate()
        node.wait()
        shutil.rmtree(datadir, ignore_errors=True)


if __name__ == "__main__":
    main()
