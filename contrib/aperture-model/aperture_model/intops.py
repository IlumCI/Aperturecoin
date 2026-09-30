# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Aperture integer inference profile v0: the exact integer operators.

This module is normative. Every node, miner and kernel must reproduce these
functions bit for bit (doc/protocol-model.md). All values are integers:

- Activations are int64 fixed point with FRAC = 16 fractional bits ("Q16").
- Weight-matmul inputs are int8 with |q| <= QMAX (Variant Z headroom for the
  PoW noise, doc/pouw-v2.md), one scale per row per GROUP input channels.
- Weights are int8 with |w| <= QMAX, one scale per output channel, stored as
  a Q30 multiplier.

Rounding is always rdiv(): round half up, i.e. floor((2a + b) / (2b)) for
b > 0. Right shifts of negative numbers are arithmetic (floor).

Weight-matmul accumulation is exact. The reference uses float64 BLAS because
every partial sum of int8 x int8 products stays below 2^53 for the dimensions
allowed by the profile (d_in <= 2^20), which makes float64 accumulation exact
and order-independent.
"""

import numpy as np

FRAC = 16
ONE = 1 << FRAC
QMAX = 95                 # 127 - r, r = 32
GROUP = 256               # activation scale group = PoW K-span g * r
WS_SHIFT = 30             # weight scale multiplier precision
LN2_Q16 = 45426           # round(ln 2 * 2^16)
EPS_Q32 = 4295            # round(1e-6 * 2^32), RMSNorm epsilon in Q32

# exp(p) for p in (-ln2, 0]: 0.3585 * (p + 1.353)^2 + 0.344 (I-BERT, arXiv 2101.01321)
EXP_A_Q16 = 23495         # round(0.3585 * 2^16)
EXP_B_Q16 = 88670         # round(1.353 * 2^16)
EXP_C_Q16 = 22544         # round(0.344 * 2^16)

I64 = np.int64


def rdiv(a, b):
    """Round-half-up integer division, b > 0 (elementwise, int64)."""
    a = np.asarray(a, dtype=I64)
    b = np.asarray(b, dtype=I64)
    return np.floor_divide(2 * a + b, 2 * b)


def isqrt(n):
    """Floor square root of non-negative int64 values below 2^62."""
    n = np.asarray(n, dtype=I64)
    r = np.sqrt(n.astype(np.float64)).astype(I64)
    for _ in range(3):
        r = np.where(r * r > n, r - 1, r)
        r = np.where((r + 1) * (r + 1) <= n, r + 1, r)
    return r


def bitlen(x):
    """Bit length of non-negative int64 values (0 for 0)."""
    x = np.asarray(x, dtype=I64)
    out = np.zeros(x.shape, dtype=I64)
    y = x.copy()
    for s in (32, 16, 8, 4, 2, 1):
        m = y >= (I64(1) << s)
        out = np.where(m, out + s, out)
        y = np.where(m, y >> s, y)
    return np.where(x > 0, out + 1, 0)


def quantize_rows(x):
    """Q16 rows -> (int8 values with |q| <= QMAX, per-row max |x| in Q16)."""
    x = np.asarray(x, dtype=I64)
    m = np.max(np.abs(x), axis=-1, keepdims=True)
    safe = np.where(m == 0, 1, m)
    q = rdiv(x * QMAX, safe)
    return q.astype(np.int8), m


def quantize_groups(x):
    """Q16 [T, d_in] -> int8 [T, d_in/GROUP, GROUP] and per-group maxima [T, d_in/GROUP, 1].

    GROUP equals the PoW K-span (g * r = 256, doc/pouw-v2.md), so every
    ticket tile is a pure int8 x int8 product within one scale group.
    """
    x = np.asarray(x, dtype=I64)
    T, d = x.shape
    assert d % GROUP == 0
    return quantize_rows(x.reshape(T, d // GROUP, GROUP))


def weight_matmul(q, m, wq_f64, ws):
    """y = dequant(q) @ W^T in Q16.

    q, m: from quantize_groups. wq_f64: int8 weights as float64 [d_out, d_in].
    ws: per-output-channel Q30 multipliers [d_out]. Each per-group acc is the
    exact int32 product that PoW tickets are computed over; groups are
    dequantized separately and summed in group order.
    """
    T, ng, G = q.shape
    out = np.zeros((T, wq_f64.shape[0]), dtype=I64)
    for g in range(ng):
        acc = (q[:, g, :].astype(np.float64) @ wq_f64[:, g * G:(g + 1) * G].T).astype(I64)
        t = rdiv(acc * ws[None, :], I64(1) << WS_SHIFT)
        out += rdiv(t * m[:, g, :], QMAX)
    return out


def rmsnorm(x, g_q16):
    """RMSNorm over the last axis. x Q16 int64, g Q16 int64 -> Q16."""
    x = np.asarray(x, dtype=I64)
    d = x.shape[-1]
    mx = np.max(np.abs(x), axis=-1, keepdims=True)
    shift = np.maximum(bitlen(mx) - 24, 0)
    xs = x >> shift
    mean = np.sum(xs * xs, axis=-1, keepdims=True) // d
    rms = isqrt(mean + (I64(EPS_Q32) >> np.minimum(2 * shift, 62)))
    rms = np.maximum(rms, 1) << shift
    return rdiv(x * g_q16, rms)


def iexp_neg(z):
    """exp(z) in Q16 for z <= 0 in Q16."""
    z = np.asarray(z, dtype=I64)
    k = np.floor_divide(-z, LN2_Q16)
    p = z + k * LN2_Q16
    t = p + EXP_B_Q16
    poly = rdiv(EXP_A_Q16 * rdiv(t * t, ONE), ONE) + EXP_C_Q16
    return np.where(k >= 62, 0, poly >> np.minimum(k, 62))


def sigmoid(x):
    x = np.asarray(x, dtype=I64)
    e = iexp_neg(-np.abs(x))
    s = rdiv(I64(ONE) * ONE, ONE + e)
    return np.where(x >= 0, s, ONE - s)


def silu(x):
    x = np.asarray(x, dtype=I64)
    return rdiv(x * sigmoid(x), ONE)


def softmax_rows(s, mask=None):
    """Softmax over the last axis of Q16 scores; masked entries get 0."""
    s = np.asarray(s, dtype=I64)
    if mask is not None:
        s = np.where(mask, s, I64(-(1 << 40)))
    mx = np.max(s, axis=-1, keepdims=True)
    e = iexp_neg(s - mx)
    if mask is not None:
        e = np.where(mask, e, 0)
    tot = np.sum(e, axis=-1, keepdims=True)
    return rdiv(e * ONE, tot)


def to_int8_direction(v):
    """Final embedding: max-abs scaled int8 (cosine similarity is scale-free)."""
    v = np.asarray(v, dtype=I64)
    m = np.max(np.abs(v), axis=-1, keepdims=True)
    return rdiv(v * 127, np.where(m == 0, 1, m)).astype(np.int8)
