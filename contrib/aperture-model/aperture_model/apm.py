# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The .apm container: one canonical file per protocol model.

Layout:
    b"APMODEL1"                      magic
    u32 LE header length
    header                           canonical JSON (sorted keys, no spaces)
    zero padding to a 64-byte boundary
    tensor data, each tensor 64-byte aligned, in header order

The header holds the architecture config, the tokenizer hash and the tensor
table [name, dtype, shape, offset, nbytes]. weights_root is BLAKE3 of the
whole file; model_id is BLAKE3("ApertureModel/v0" || weights_root).
"""

import json
import struct

import numpy as np

try:
    from blake3 import blake3 as _blake3
except ImportError:  # pragma: no cover
    _blake3 = None

MAGIC = b"APMODEL1"
ALIGN = 64
DTYPES = {"int8": np.int8, "int32": np.int32, "int64": np.int64}


def blake3_hex(data):
    return _blake3(data).hexdigest()


def blake3_file(path):
    h = _blake3()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 24), b""):
            h.update(chunk)
    return h.hexdigest()


def _canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()


def _pad(n):
    return (-n) % ALIGN


def write(path, config, tensors):
    """tensors: list of (name, np.ndarray) with dtypes in DTYPES."""
    table = []
    offset = 0
    for name, arr in tensors:
        dt = str(arr.dtype)
        assert dt in DTYPES, (name, dt)
        table.append([name, dt, list(arr.shape), offset, arr.nbytes])
        offset += arr.nbytes + _pad(arr.nbytes)
    header = _canonical({"config": config, "tensors": table})
    prefix = MAGIC + struct.pack("<I", len(header)) + header
    with open(path, "wb") as f:
        f.write(prefix + b"\0" * _pad(len(prefix)))
        for _, arr in tensors:
            data = np.ascontiguousarray(arr).astype(arr.dtype.newbyteorder("<"), copy=False).tobytes()
            f.write(data + b"\0" * _pad(len(data)))
    root = blake3_file(path)
    return root, model_id(root)


def model_id(weights_root_hex):
    return blake3_hex(b"ApertureModel/v0" + bytes.fromhex(weights_root_hex))


def read(path):
    """Return (config, {name: array}) using a read-only memory map."""
    with open(path, "rb") as f:
        head = f.read(12)
        assert head[:8] == MAGIC, "not an .apm file"
        (hlen,) = struct.unpack("<I", head[8:])
        header = json.loads(f.read(hlen))
    base = 12 + hlen + _pad(12 + hlen)
    mm = np.memmap(path, dtype=np.uint8, mode="r")
    arrays = {}
    for name, dt, shape, off, nbytes in header["tensors"]:
        raw = mm[base + off: base + off + nbytes]
        arrays[name] = raw.view(np.dtype(DTYPES[dt]).newbyteorder("<")).reshape(shape)
    return header["config"], arrays
