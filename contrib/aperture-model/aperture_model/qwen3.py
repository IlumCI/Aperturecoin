# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Qwen3-Embedding forward passes: float32 reference and the integer profile.

The float pass follows the Hugging Face Qwen3 implementation and is used only
to measure conversion quality. The integer pass is the normative protocol
model computation (profile "aperture-int-v0", doc/protocol-model.md).
"""

import numpy as np

from . import apm
from . import intops as I
from .hf import SafeTensors

PROJ = ("q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj")
MAX_POS = 512


def _proj_key(i, p):
    part = "mlp" if p in ("gate_proj", "up_proj", "down_proj") else "self_attn"
    return f"layers.{i}.{part}.{p}.weight"


def rope_tables(head_dim, theta, max_pos):
    inv = 1.0 / (theta ** (np.arange(0, head_dim, 2, dtype=np.float64) / head_dim))
    ang = np.arange(max_pos, dtype=np.float64)[:, None] * inv[None, :]
    return np.cos(ang), np.sin(ang)


# ---------------------------------------------------------------- float32 --

class FloatModel:
    def __init__(self, st_path, cfg):
        self.st = SafeTensors(st_path)
        self.c = cfg
        self.embed = self.st.get("embed_tokens.weight")
        cos, sin = rope_tables(cfg["head_dim"], cfg["rope_theta"], MAX_POS)
        self.cos, self.sin = cos.astype(np.float32), sin.astype(np.float32)

    def w(self, k):
        return self.st.get(k)

    @staticmethod
    def rms(x, g, eps):
        v = np.mean(x.astype(np.float32) ** 2, axis=-1, keepdims=True)
        return (x / np.sqrt(v + eps)) * g

    def rope(self, x, T):
        h = x.shape[-1] // 2
        c, s = self.cos[:T, None, :], self.sin[:T, None, :]
        x1, x2 = x[..., :h], x[..., h:]
        return np.concatenate([x1 * c - x2 * s, x2 * c + x1 * s], axis=-1)

    def embed_ids(self, ids):
        c = self.c
        T, H, KV, D = len(ids), c["num_attention_heads"], c["num_key_value_heads"], c["head_dim"]
        eps = c["rms_norm_eps"]
        x = self.embed[ids].astype(np.float32)
        mask = np.tril(np.ones((T, T), dtype=bool))
        for i in range(c["num_hidden_layers"]):
            p = f"layers.{i}."
            h = self.rms(x, self.w(p + "input_layernorm.weight"), eps)
            q = (h @ self.w(_proj_key(i, "q_proj")).T).reshape(T, H, D)
            k = (h @ self.w(_proj_key(i, "k_proj")).T).reshape(T, KV, D)
            v = (h @ self.w(_proj_key(i, "v_proj")).T).reshape(T, KV, D)
            q = self.rope(self.rms(q, self.w(p + "self_attn.q_norm.weight"), eps), T)
            k = self.rope(self.rms(k, self.w(p + "self_attn.k_norm.weight"), eps), T)
            out = np.empty((T, H, D), dtype=np.float32)
            for hh in range(H):
                kv = hh // (H // KV)
                s = (q[:, hh] @ k[:, kv].T) / np.sqrt(D)
                s = np.where(mask, s, -np.inf)
                s = np.exp(s - s.max(axis=-1, keepdims=True))
                out[:, hh] = (s / s.sum(axis=-1, keepdims=True)) @ v[:, kv]
            x = x + out.reshape(T, H * D) @ self.w(_proj_key(i, "o_proj")).T
            h = self.rms(x, self.w(p + "post_attention_layernorm.weight"), eps)
            g = h @ self.w(_proj_key(i, "gate_proj")).T
            u = h @ self.w(_proj_key(i, "up_proj")).T
            x = x + ((g / (1 + np.exp(-np.clip(g, -80, 80)))) * u) @ self.w(_proj_key(i, "down_proj")).T
        x = self.rms(x, self.w("norm.weight"), eps)
        e = x[-1]
        return e / np.linalg.norm(e)


# ---------------------------------------------------------------- convert --

def convert(st_path, hf_cfg, tokenizer_path, out_path):
    """Quantize a Hugging Face Qwen3-Embedding checkpoint to an .apm file."""
    st = SafeTensors(st_path)
    D = hf_cfg["head_dim"]
    config = {
        "profile": "aperture-int-v0",
        "arch": "qwen3-embedding",
        "hidden_size": hf_cfg["hidden_size"],
        "intermediate_size": hf_cfg["intermediate_size"],
        "num_hidden_layers": hf_cfg["num_hidden_layers"],
        "num_attention_heads": hf_cfg["num_attention_heads"],
        "num_key_value_heads": hf_cfg["num_key_value_heads"],
        "head_dim": D,
        "vocab_size": hf_cfg["vocab_size"],
        "max_positions": MAX_POS,
        "eos_token_id": hf_cfg["eos_token_id"],
        "pooling": "last_token",
        "qmax": I.QMAX,
        "tokenizer_blake3": apm.blake3_file(tokenizer_path),
        "source": "Qwen/Qwen3-Embedding-0.6B (Apache-2.0)",
    }
    tensors = []

    def q16(name, key):
        tensors.append((name, np.rint(st.get(key).astype(np.float64) * I.ONE).astype(np.int64)))

    def weight(name, w, qmax):
        w = w.astype(np.float64)
        s = np.max(np.abs(w), axis=1) / qmax
        s = np.where(s == 0, 1.0, s)
        tensors.append((name + ".q", np.clip(np.rint(w / s[:, None]), -qmax, qmax).astype(np.int8)))
        tensors.append((name + ".s", np.rint(s * (1 << I.WS_SHIFT)).astype(np.int64)))

    weight("embed", st.get("embed_tokens.weight"), 127)
    for i in range(hf_cfg["num_hidden_layers"]):
        p = f"layers.{i}."
        q16(p + "input_norm", p + "input_layernorm.weight")
        q16(p + "post_norm", p + "post_attention_layernorm.weight")
        q16(p + "q_norm", p + "self_attn.q_norm.weight")
        q16(p + "k_norm", p + "self_attn.k_norm.weight")
        for pr in PROJ:
            weight(p + pr, st.get(_proj_key(i, pr)), I.QMAX)
    q16("norm", "norm.weight")
    cos, sin = rope_tables(D, hf_cfg["rope_theta"], MAX_POS)
    tensors.append(("rope.cos", np.rint(cos * I.ONE).astype(np.int64)))
    tensors.append(("rope.sin", np.rint(sin * I.ONE).astype(np.int64)))
    return apm.write(out_path, config, tensors)


class _Xof:
    def __init__(self, seed):
        self.h = apm._blake3(seed)
        self.pos = 0

    def read(self, n):
        out = self.h.digest(length=n, seek=self.pos)
        self.pos += n
        return out


def tiny_model(out_path, seed=1):
    """Deterministic random-weight model in the same architecture, for regtest
    and test vectors. Tokenizer: raw UTF-8 bytes (ids 0..255), EOS = 256.

    All values come from BLAKE3-XOF("ApertureTinyModel/v0" || seed as u32 LE),
    read sequentially, so other implementations can regenerate the model
    without this code: int8 weight = (byte mod (2*qmax+1)) - qmax; scale =
    2^20 + (u32 LE mod 3*2^20); norm = ONE/2 + (u32 LE mod 3*ONE/2).
    """
    xof = _Xof(b"ApertureTinyModel/v0" + int(seed).to_bytes(4, "little"))
    H, L, NH, KV, D, FF, V = 256, 2, 2, 1, 128, 768, 257
    config = {
        "profile": "aperture-int-v0", "arch": "qwen3-embedding",
        "hidden_size": H, "intermediate_size": FF, "num_hidden_layers": L,
        "num_attention_heads": NH, "num_key_value_heads": KV, "head_dim": D,
        "vocab_size": V, "max_positions": 64, "eos_token_id": 256,
        "pooling": "last_token", "qmax": I.QMAX, "tokenizer": "bytes",
        "tokenizer_blake3": apm.blake3_hex(b"bytes"), "source": f"tiny_model(seed={seed})",
    }
    tensors = []

    def weight(name, dout, din, qmax):
        b = np.frombuffer(xof.read(dout * din), dtype=np.uint8).astype(np.int64)
        tensors.append((name + ".q", ((b % (2 * qmax + 1)) - qmax).astype(np.int8).reshape(dout, din)))
        u = np.frombuffer(xof.read(4 * dout), dtype="<u4").astype(np.int64)
        tensors.append((name + ".s", (1 << 20) + u % (3 << 20)))

    def norm(name, d):
        u = np.frombuffer(xof.read(4 * d), dtype="<u4").astype(np.int64)
        tensors.append((name, I.ONE // 2 + u % (3 * I.ONE // 2)))

    weight("embed", V, H, 127)
    shapes = {"q_proj": (NH * D, H), "k_proj": (KV * D, H), "v_proj": (KV * D, H), "o_proj": (H, NH * D),
              "gate_proj": (FF, H), "up_proj": (FF, H), "down_proj": (H, FF)}
    for i in range(L):
        p = f"layers.{i}."
        norm(p + "input_norm", H)
        norm(p + "post_norm", H)
        norm(p + "q_norm", D)
        norm(p + "k_norm", D)
        for pr in PROJ:
            weight(p + pr, *shapes[pr], I.QMAX)
    norm("norm", H)
    cos, sin = rope_tables(D, 1e6, 64)
    tensors.append(("rope.cos", np.rint(cos * I.ONE).astype(np.int64)))
    tensors.append(("rope.sin", np.rint(sin * I.ONE).astype(np.int64)))
    return apm.write(out_path, config, tensors)


# ---------------------------------------------------------------- integer --

class IntModel:
    """Normative integer forward pass over an .apm model."""

    def __init__(self, apm_path):
        self.c, self.t = apm.read(apm_path)
        self.isq = int(round(I.ONE / np.sqrt(self.c["head_dim"])))
        self._wcache = {}

    def W(self, name):
        if name not in self._wcache:
            self._wcache[name] = (self.t[name + ".q"].astype(np.float64), np.asarray(self.t[name + ".s"], dtype=np.int64))
        return self._wcache[name]

    def linear(self, x, name, trace=None):
        q, m = I.quantize_groups(x)
        wq, ws = self.W(name)
        if trace is not None:
            trace.append((name, q, m))
        return I.weight_matmul(q, m, wq, ws)

    def rope(self, x, T):
        h = x.shape[-1] // 2
        c = self.t["rope.cos"][:T, None, :].astype(np.int64)
        s = self.t["rope.sin"][:T, None, :].astype(np.int64)
        x1, x2 = x[..., :h], x[..., h:]
        return np.concatenate([I.rdiv(x1 * c - x2 * s, I.ONE), I.rdiv(x2 * c + x1 * s, I.ONE)], axis=-1)

    def embed_ids(self, ids, trace=None):
        """ids: token ids ending with EOS. Returns the int8 embedding [hidden]."""
        c = self.c
        T = len(ids)
        assert 0 < T <= c["max_positions"]
        H, KV, D = c["num_attention_heads"], c["num_key_value_heads"], c["head_dim"]
        ids = np.asarray(ids)
        eq = self.t["embed.q"][ids].astype(np.int64)
        es = np.asarray(self.t["embed.s"][ids], dtype=np.int64)
        x = I.rdiv(eq * es[:, None], 1 << (I.WS_SHIFT - I.FRAC))
        mask = np.tril(np.ones((T, T), dtype=bool))
        for i in range(c["num_hidden_layers"]):
            p = f"layers.{i}."
            h = I.rmsnorm(x, self.t[p + "input_norm"])
            q = self.linear(h, p + "q_proj", trace).reshape(T, H, D)
            k = self.linear(h, p + "k_proj", trace).reshape(T, KV, D)
            v = self.linear(h, p + "v_proj", trace).reshape(T, KV, D)
            q = self.rope(I.rmsnorm(q, self.t[p + "q_norm"]), T)
            k = self.rope(I.rmsnorm(k, self.t[p + "k_norm"]), T)
            out = np.empty((T, H, D), dtype=np.int64)
            for hh in range(H):
                kv = hh // (H // KV)
                s = I.rdiv(I.rdiv(q[:, hh] @ k[:, kv].T, I.ONE) * self.isq, I.ONE)
                pr = I.softmax_rows(s, mask)
                out[:, hh] = I.rdiv(pr @ v[:, kv], I.ONE)
            x = x + self.linear(out.reshape(T, H * D), p + "o_proj", trace)
            h = I.rmsnorm(x, self.t[p + "post_norm"])
            g = self.linear(h, p + "gate_proj", trace)
            u = self.linear(h, p + "up_proj", trace)
            x = x + self.linear(I.rdiv(I.silu(g) * u, I.ONE), p + "down_proj", trace)
        x = I.rmsnorm(x, self.t["norm"])
        return I.to_int8_direction(x[-1])
