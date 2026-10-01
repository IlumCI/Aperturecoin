#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Install the protocol model that testnet useful-work mining runs.

Downloads the pinned Hugging Face checkpoint, checks every file's SHA-256,
converts it to the integer profile (.apm), checks the resulting model_id
against the chain's, and installs it where the node looks by default:

    <datadir>/models/<model_id>.apm

Usage:
    fetch_protocol_model.py [--datadir=DIR] [--workdir=DIR] [--apm=FILE]

--apm installs an already converted file after checking its model_id, so a
converted model can be copied between machines without re-downloading.
"""

import argparse
import hashlib
import os
import shutil
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Placeholder protocol model (doc/protocol-model.md).
REPO = "Qwen/Qwen3-Embedding-0.6B"
REVISION = "97b0c614be4d77ee51c0cef4e5f07c00f9eb65b3"
FILES = {
    "config.json": "b5bf1f51fc45be473a54718cef92448d90a1be001bf9b9a44b8c7f10a19feaa9",
    "tokenizer.json": "def76fb086971c7867b829c23a26261e38d9d74e02139253b38aeb9df8b4b50a",
    "model.safetensors": "0437e45c94563b09e13cb7a64478fc406947a93cb34a7e05870fc8dcd48e23fd",
}
MODEL_ID = "036e18a4393ab94c024da544ca7298358b4b937d8d777d02ff6cda4de95fe626"


def default_datadir():
    if sys.platform == "win32":
        return os.path.join(os.environ["APPDATA"], "Aperture")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/Aperture")
    return os.path.expanduser("~/.aperture")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(workdir):
    os.makedirs(workdir, exist_ok=True)
    for name, digest in FILES.items():
        path = os.path.join(workdir, name)
        if not (os.path.exists(path) and sha256_file(path) == digest):
            url = f"https://huggingface.co/{REPO}/resolve/{REVISION}/{name}"
            print(f"downloading {url}", flush=True)
            with urllib.request.urlopen(url) as r, open(path + ".part", "wb") as f:
                shutil.copyfileobj(r, f, 1 << 20)
            os.replace(path + ".part", path)
        got = sha256_file(path)
        if got != digest:
            sys.exit(f"{name}: sha256 {got}, expected {digest}")
        print(f"ok {name}")


def model_id_of(path):
    from aperture_model import apm
    return apm.model_id(apm.blake3_file(path))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--datadir", default=default_datadir())
    ap.add_argument("--workdir", default=None, help="download and conversion directory (default: <datadir>/models/src)")
    ap.add_argument("--apm", default=None, help="install this converted file instead of converting")
    args = ap.parse_args()

    store = os.path.join(args.datadir, "models")
    os.makedirs(store, exist_ok=True)
    dest = os.path.join(store, MODEL_ID + ".apm")

    if args.apm:
        src = args.apm
    else:
        import json
        from aperture_model.qwen3 import convert
        work = args.workdir or os.path.join(store, "src")
        fetch(work)
        with open(os.path.join(work, "config.json")) as f:
            cfg = json.load(f)
        src = os.path.join(work, "model.apm")
        print("converting to the integer profile", flush=True)
        convert(os.path.join(work, "model.safetensors"), cfg, os.path.join(work, "tokenizer.json"), src)

    mid = model_id_of(src)
    if mid != MODEL_ID:
        sys.exit(f"{src}: model_id {mid}, expected {MODEL_ID}")
    if os.path.abspath(src) != os.path.abspath(dest):
        shutil.copyfile(src, dest + ".part")
        os.replace(dest + ".part", dest)
    print(f"installed {dest}")


if __name__ == "__main__":
    main()
