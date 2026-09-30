# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Minimal safetensors reader (bf16/f16/f32 -> float32) without torch."""

import json
import struct

import numpy as np


class SafeTensors:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            (n,) = struct.unpack("<Q", f.read(8))
            self.header = json.loads(f.read(n))
        self.header.pop("__metadata__", None)
        self.base = 8 + n
        self.mm = np.memmap(path, dtype=np.uint8, mode="r")

    def keys(self):
        return list(self.header)

    def get(self, name):
        meta = self.header[name]
        start, end = meta["data_offsets"]
        raw = self.mm[self.base + start: self.base + end]
        shape = meta["shape"]
        dt = meta["dtype"]
        if dt == "BF16":
            u = raw.view("<u2").astype(np.uint32) << 16
            return u.view(np.float32).reshape(shape)
        if dt == "F16":
            return raw.view("<f2").astype(np.float32).reshape(shape)
        if dt == "F32":
            return np.array(raw.view("<f4").reshape(shape))
        raise ValueError(f"unsupported dtype {dt}")
