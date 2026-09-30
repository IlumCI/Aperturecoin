#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Compare the integer protocol model against the float reference.

Usage: quality_report.py <hf_dir> <model.apm>
Reports per-text cosine(float, int), agreement of the pairwise similarity
ranking, top-1 retrieval agreement and timings.
"""

import json
import os
import sys
import time

import numpy as np
from tokenizers import Tokenizer

from aperture_model.qwen3 import FloatModel, IntModel

QUERY = "Instruct: Given a web search query, retrieve relevant passages that answer the query\nQuery:"
PAIRS = [
    ("What is the capital of France?", "Paris is the capital and largest city of France."),
    ("How do vaccines work?", "Vaccines train the immune system to recognize a pathogen without causing disease."),
    ("Explain proof of work", "In proof of work, miners compete to find a nonce whose block hash is below a target."),
    ("Best way to cook rice", "Rinse the rice, then simmer it covered with twice its volume of water for 18 minutes."),
    ("What causes tides?", "Tides are caused by the gravitational pull of the Moon and the Sun on the oceans."),
    ("matrix multiplication on GPUs", "Tensor cores accelerate dense matrix multiply-accumulate operations in low precision."),
    ("symptoms of dehydration", "Thirst, dark urine, dizziness and fatigue are common signs of dehydration."),
    ("Who wrote Hamlet?", "Hamlet is a tragedy written by William Shakespeare around 1600."),
]


def spearman(a, b):
    ra, rb = np.argsort(np.argsort(a)), np.argsort(np.argsort(b))
    return float(np.corrcoef(ra, rb)[0, 1])


def main():
    hf_dir, apm_path = sys.argv[1], sys.argv[2]
    with open(os.path.join(hf_dir, "config.json")) as f:
        cfg = json.load(f)
    tok = Tokenizer.from_file(os.path.join(hf_dir, "tokenizer.json"))
    fm, im = FloatModel(os.path.join(hf_dir, "model.safetensors"), cfg), IntModel(apm_path)
    eos = im.c["eos_token_id"]
    texts = [QUERY + q for q, _ in PAIRS] + [d for _, d in PAIRS]
    fe, ie, cos, tf, ti = [], [], [], 0.0, 0.0
    for t in texts:
        ids = tok.encode(t).ids + [eos]
        t0 = time.time(); f = fm.embed_ids(ids); t1 = time.time()
        i = im.embed_ids(ids).astype(np.float64); t2 = time.time()
        i /= np.linalg.norm(i)
        tf += t1 - t0; ti += t2 - t1
        fe.append(f); ie.append(i); cos.append(float(f @ i))
    fe, ie = np.array(fe), np.array(ie)
    n = len(PAIRS)
    sf, si = fe[:n] @ fe[n:].T, ie[:n] @ ie[n:].T
    iu = np.triu_indices(len(texts), 1)
    report = {
        "texts": len(texts),
        "cosine_float_vs_int": {"min": min(cos), "mean": float(np.mean(cos))},
        "query_doc_similarity_spearman": spearman(sf.ravel(), si.ravel()),
        "all_pairs_similarity_spearman": spearman((fe @ fe.T)[iu], (ie @ ie.T)[iu]),
        "top1_float_correct": int(np.sum(np.argmax(sf, axis=1) == np.arange(n))),
        "top1_int_correct": int(np.sum(np.argmax(si, axis=1) == np.arange(n))),
        "top1_agreement": int(np.sum(np.argmax(sf, axis=1) == np.argmax(si, axis=1))),
        "seconds_float": round(tf, 1),
        "seconds_int": round(ti, 1),
    }
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
