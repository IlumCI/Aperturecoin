Protocol model and the Aperture integer inference profile
==========================================================

Status: the reference implementation is in Python (`contrib/aperture-model`)
and the node implementation in C++ (`src/model`). The two agree bit for bit
on the golden vectors. Consensus integration is described in
`doc/pouw-v2.md`.

Mining runs the network's **protocol model** (`doc/pouw-v2.md`). Every node,
miner and GPU kernel must compute that model **bit for bit** identically,
because proof-of-work tickets and fraud proofs compare exact integers. This
document defines the model format and the integer arithmetic ("profile") that
makes that possible.

Launch model
------------

Qwen3-Embedding-0.6B is the protocol model for testnet and mainnet launch.
Why this model:

- **License.** Apache-2.0, so every node may store and redistribute the
  weights.
- **Verification cost.** At 0.6 B parameters (600 MB in the profile) every
  full node can recompute every result on a CPU: one 512-token request takes
  22 s on four cores, and the per-block budget of 1,024 tokens bounds a full
  block. A larger model would push full verification to GPUs and shrink the
  set of people who can run a validating node.
- **Quality.** It is a current multilingual retrieval model, and the integer
  profile keeps it close to the float reference (table below: Spearman
  0.97–0.98, identical top-1 retrieval).
- **Exactness.** The architecture is fully supported by the integer profile,
  so results are bit-exact across CPUs and GPUs.

A replacement (a project-trained model, or a newer open model) is a
scheduled upgrade to a new `model_id` at a height announced in a release.
Training on chain is specified separately and needs a human-gated weight
activation (see the training notes in `doc/pouw-v2.md`).

| | |
|---|---|
| Source | [Qwen/Qwen3-Embedding-0.6B](https://huggingface.co/Qwen/Qwen3-Embedding-0.6B), Apache-2.0 |
| Architecture | decoder-only transformer: 28 layers, hidden 1024, 16 query heads and 8 KV heads of 128, SwiGLU 3072, RMSNorm, QK-norm, RoPE (θ = 1e6), last-token pooling |
| Weight matmul work | 440 M MACs per token (all ticket-eligible) |
| .apm file | 600,754,432 bytes (int8) |
| weights_root | `87815c21d3dd95354f91eaa65c9fcd1b626dc3f323edb6fb44786586d4abf67f` |
| model_id | `036e18a4393ab94c024da544ca7298358b4b937d8d777d02ff6cda4de95fe626` |
| Max sequence | 512 tokens (profile limit for v0) |
| Output | 1024-dim int8 direction vector (cosine similarity) |

Conversion quality against the float32 reference
(`contrib/aperture-model/quality_report.py`, 16 texts, 8 query/passage
pairs):

| Metric | Value |
|---|---|
| cosine(float, int), mean / min | 0.963 / 0.952 |
| Spearman ρ, query × passage similarities | 0.970 |
| Spearman ρ, all-pairs similarities | 0.980 |
| top-1 retrieval: float / int / agreement | 8/8 / 8/8 / 8/8 |

**How the error was reduced.** Measured by ablation:
- Per-row activation quantization dominated the error, with a mean cosine of
  0.943.
- Scaling activations per group of 256 input channels brought it to 0.963.
  256 is the ticket K-span width, so each ticket tile stays one pure
  int8 × int8 product.
- Per-group weight scales did not help. The rest of the error comes from
  95-level int8 weights and the polynomial exponential.
- A model trained inside the profile (below) removes both.

**End to end on a node** (`contrib/aperture-model/demo_regtest.py`). A regtest
node was run with `-powv2height=102 -protocolmodel=qwen3-embed-0.6b.apm`:
1. A wallet paid for 5 embedding requests (`sendembeddingrequest`, Qwen
   token ids).
2. `generatetoaddress` mined them in 6.9 s. That covers the forward pass,
   the ticket search over the real activations, and validation.
3. All 5 embeddings served in the coinbase equal the Python integer
   reference bit for bit.
4. `searchembeddings` over the mined results returned the correct passage
   for 4 of 4 natural-language queries.

Replacing the launch model is a scheduled upgrade to a new `model_id` at a
height announced in a release.

The .apm format
---------------

This is one canonical file per model (`aperture_model/apm.py`):

```
"APMODEL1" | u32 header_len | canonical JSON header | pad64 | tensors (each pad64)
```

The header holds the architecture config, `tokenizer_blake3`, and a tensor
table of `[name, dtype, shape, offset, nbytes]`. Dtypes are int8, int32 and
int64, all little-endian.

- `weights_root` = BLAKE3 of the file.
- `model_id` = `BLAKE3("ApertureModel/v0" || weights_root)`.

A node checks its model file against `weights_root` once at startup.

Profile `aperture-int-v0`
-------------------------

The normative definition is `aperture_model/intops.py` together with
`IntModel` in `aperture_model/qwen3.py`. In summary:

- **Numbers.**
  - Activations are int64 fixed point, Q16.
  - Weights are int8 with |w| ≤ 95 (PoW Variant Z headroom), one Q30
    multiplier per output channel.
  - Norm weights and RoPE tables are Q16 int64, stored in the file.
- **Rounding.** `rdiv(a, b) = floor((2a + b) / 2b)` everywhere. Shifts are
  arithmetic.
- **Weight matmul.**
  1. Split the input row into groups of 256 channels.
  2. Quantize each group to int8 with |q| ≤ 95 against its max |x|.
  3. Take the exact integer product per group, the `acc` that tickets are
     computed over.
  4. Dequantize per group, `rdiv(rdiv(acc·ws, 2^30)·m, 95)`, and sum in group
     order.
- **RMSNorm.** Pre-shift to 24 bits, integer mean of squares, floor `isqrt`,
  ε = 2^-32 × 4295.
- **exp.** Range reduction by ln 2, then the I-BERT second-order polynomial
  (arXiv 2101.01321). Maximum error is below 0.01 on [−20, 0].
- **Other nonlinearities.** Sigmoid, SiLU and softmax are built from that
  integer exp and `rdiv`.
- **Attention.** Q·K and P·V are exact int64 products, and carry no tickets.
- **Output.** The final RMSNorm, the last token, and max-abs scaling to int8.

Weight-matmul accumulation in the reference uses float64 BLAS. Every partial
sum of int8 × int8 products stays below 2^53, so this is exact and does not
depend on summation order. Nodes use int32 accumulation instead.

**Test vectors.** `contrib/aperture-model/test/test_intprofile.py` holds
golden hashes for a tiny model. That model is generated from BLAKE3-XOF, so
any implementation can regenerate it without numpy. The C++ port must
reproduce the hashes exactly.

Requirements for the project model (CoCoNut-style latent model)
---------------------------------------------------------------

The planned model is:
- about 0.1–0.4B parameters, about 200 MB;
- trained from scratch;
- reasoning in continuous latent space, CoCoNut-style (arXiv 2412.06769),
  with a natural-language encoder/decoder harness around it;
- equipped with an unbounded-context memory ("infContext");
- about as expensive to run as a 14B dense model, because of latent
  recurrence.

The profile places these requirements on it:

1. **Train inside the profile.** The latent loop feeds each final hidden
   state back as the next input embedding, so quantization error compounds
   across latent steps. Post-training conversion, as done for the
   placeholder, will not hold up over long latent chains. The training
   forward pass must be the integer profile (quantization-aware training
   with straight-through gradients). The deployed integer model then *is* the
   trained model, not an approximation of it.
2. **Weight matmuls must dominate compute.** Only weight matmuls carry
   tickets. Latent recurrence re-applies the same weights many times, which
   is ideal: 200 MB of weights and 14B-class work give a very high ticket
   yield per byte a node stores.
   - Attention products and memory updates carry no tickets. If they become
     a large share of compute, the useful share of mining falls.
3. **Integer, bounded memory state.** If infContext is a compressive memory
   (for example Infini-attention, arXiv 2404.07143: a linear-attention matrix
   updated per segment), its accumulators must be integer. They also need
   bounded growth (normalized or decayed), fixed in the profile, so results
   stay bit-exact over arbitrary context lengths.
4. **Bounded compute per request.** Blocks and fraud proofs need a known
   upper bound on the work in one request: the latent step count and context
   length must be capped. The fee is priced per latent step, not per input
   token.
5. **Group-aligned widths.** Every weight matmul input width must be a
   multiple of 256.
6. **Fraud-proof granularity.** A single-step check recomputes one op on one
   row. With latent recurrence, act_root must commit to activations at every
   latent step, so the bisection can reach any step.

Open questions for the model owner:
- the exact infContext mechanism;
- the latent step count per request, and whether it is fixed or adaptive;
- whether the on-chain output is the final latent state, a pooled embedding,
  or decoded text.

Decoded text would require the decoder in the profile, plus sampling rules
(greedy or seeded).
