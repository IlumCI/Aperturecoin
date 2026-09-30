#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Convert a Hugging Face Qwen3-Embedding directory to an .apm protocol model.

Usage: convert_qwen3.py <hf_dir> <out.apm>
<hf_dir> must hold config.json, model.safetensors and tokenizer.json.
Prints weights_root and model_id (doc/protocol-model.md).
"""

import json
import os
import sys

from aperture_model.qwen3 import convert


def main():
    src, out = sys.argv[1], sys.argv[2]
    with open(os.path.join(src, "config.json")) as f:
        cfg = json.load(f)
    root, mid = convert(os.path.join(src, "model.safetensors"), cfg, os.path.join(src, "tokenizer.json"), out)
    print(json.dumps({"file": out, "bytes": os.path.getsize(out), "weights_root": root, "model_id": mid}, indent=2))


if __name__ == "__main__":
    main()
