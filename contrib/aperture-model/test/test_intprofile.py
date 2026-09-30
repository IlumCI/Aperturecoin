#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Golden vectors for the integer profile (tiny model, byte tokenizer).

Any implementation of the profile (C++ node, GPU miners) must reproduce these.
Run: python3 test/test_intprofile.py
"""

import os
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from aperture_model import apm, intops as I  # noqa: E402
from aperture_model.qwen3 import IntModel, tiny_model  # noqa: E402

GOLDEN = {
    "weights_root": "75ed16ed07b559919dd343b24d72c06a6ae582bb55278a81264012a151e46c17",
    "model_id": "f4cb7bf674eedb6bec59de9f06510ecd42599ab4a903366c4c22b56e6c2bc31d",
    "embeddings": {
        "": "974e6dd48a44d827fee01618ad45baaacc55bbe712dc9efbaa102d6d5615695d",
        "ApertureCoin": "20857c80a0bb06d05653a933ae14062649a95c57017ed190c227791bd34111ed",
        "The quick brown fox jumps over the lazy dog.": "de035429b91db700b5161e63431a8406b6d443b20d0ff4bfddf13e34dcea9eae",
    },
}
TEXTS = ["", "ApertureCoin", "The quick brown fox jumps over the lazy dog."]


def ids_of(text):
    return list(text.encode()) + [256]


class IntProfileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.path = os.path.join(cls.dir.name, "tiny.apm")
        cls.root, cls.mid = tiny_model(cls.path)
        cls.model = IntModel(cls.path)

    def test_ops(self):
        self.assertEqual(I.rdiv(5, 2).item(), 3)
        self.assertEqual(I.rdiv(-5, 2).item(), -2)
        self.assertEqual(I.rdiv(-7, 2).item(), -3)
        n = np.array([0, 1, 2, 3, 4, 15, 16, (1 << 62) - 1], dtype=np.int64)
        r = I.isqrt(n)
        self.assertTrue(np.all(r * r <= n) and np.all((r + 1) * (r + 1) > n))
        z = np.arange(-20 * I.ONE, 1, 997, dtype=np.int64)
        err = np.abs(I.iexp_neg(z) / I.ONE - np.exp(z / I.ONE))
        self.assertLess(err.max(), 0.01)

    def test_golden(self):
        self.assertEqual(self.root, GOLDEN["weights_root"])
        self.assertEqual(self.mid, GOLDEN["model_id"])
        for t in TEXTS:
            e = self.model.embed_ids(ids_of(t))
            self.assertEqual(apm.blake3_hex(e.tobytes()), GOLDEN["embeddings"][t], t)

    def test_deterministic(self):
        a = self.model.embed_ids(ids_of("repeat"))
        b = IntModel(self.path).embed_ids(ids_of("repeat"))
        self.assertTrue(np.array_equal(a, b))


if __name__ == "__main__":
    if "--print-golden" in sys.argv:
        with tempfile.TemporaryDirectory() as d:
            root, mid = tiny_model(os.path.join(d, "t.apm"))
            m = IntModel(os.path.join(d, "t.apm"))
            print(repr({"weights_root": root, "model_id": mid,
                        "embeddings": {t: apm.blake3_hex(m.embed_ids(ids_of(t)).tobytes()) for t in TEXTS}}))
        sys.exit(0)
    unittest.main()
