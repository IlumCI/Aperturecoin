ApertureMatMul v2: proof of useful work by protocol-model inference (specification draft)
======================================================================================

Status: implemented and tested on regtest (see "Implementation status"
below). It is activated with `-powv2height=<n>` and uses the built-in tiny
protocol model. Mainnet and testnet stay on v1 until the protocol model and
`(r, g)` are fixed. v2 replaces v1 there before any public network launches.

Decisions already taken:
- **Mining runs the network's own model.** The first workload is embedding
  inference; training comes in a later upgrade.
- **The protocol model is supplied by the project.** Its architecture and
  weights are pinned in consensus. Until the project's own model is ready,
  the placeholder is Qwen3-Embedding-0.6B (Apache-2.0), converted to the
  integer profile. The format, profile and conversion quality are in
  `doc/protocol-model.md`.
- **The model is at most about 1B parameters, about 1 GB in int8.** Every full
  node stores the weights.

Idea
----

The chain has one **protocol model** M. Its int8 weights are committed in
consensus as `weights_root`.

- Users submit raw inputs (text or raw vectors) in **embedding requests**,
  with a fee.
- Mining means running M's forward pass over a batch of pending requests.
- Every weight matrix multiplication in that pass is computed in the
  noisy, tiled form of Komargodski–Weinstein
  ([arXiv:2504.09971](https://arxiv.org/abs/2504.09971)), so the pass emits
  proof-of-work lottery tickets as a by-product.
- When a ticket meets the target, the miner finishes the pass and publishes a
  block. The block carries the batch's **final embeddings** and the winning
  ticket.

What consensus enforces, and what it does not:

| Property | Enforced by |
|---|---|
| Every ticket is a product with the **real protocol weights** | Consensus. Nodes hold the weights and check the ticket's weight panel against `weights_root`. |
| Tickets cannot be shortcut, whatever the activations | KW Assumption 6.4 (Variant Q) or its integer analogue A1 (Variant Z). Security holds for any left-hand matrix. |
| The embeddings in a block are correct | Fraud proofs. Request fees mature after a challenge window, and a single-step fraud proof forfeits them. |
| The activations came from real user inputs | Not enforced. A miner can run the real model on its own junk inputs. That earns no request fees, and the useful share is measured on chain. |

This closes most of the gap measured for a deployed matmul proof-of-useful-work
(PoUW) network ([arXiv:2606.04819](https://arxiv.org/abs/2606.04819)), where
miners multiplied random matrices. Here, half of every mined product is the
model's weights, fixed by consensus.

Protocol model requirements
---------------------------

The supplied model must be converted to the **Aperture integer inference
profile**, so that every node, CPU and GPU produce bit-identical results.
Fraud proofs depend on this.

- **Size.** At most 1.0e9 parameters. Embedding dimension at most 4096.
  Maximum sequence length is fixed in the model spec.
- **Weight layers.** Weights are int8, with integer requantization between
  layers (multiplier and shift, no floating point). Accumulation is exact
  int32. Activations fed to weight matmuls are int8 with
  |a| ≤ β = 127 − r (Variant Z headroom), with **one scale per 256 input
  channels, which is exactly one ticket K-span (g·r)**. So every ticket tile
  is a pure int8 product within one scale group, and dequantization happens
  after the ticket. The normative definition is in `doc/protocol-model.md`.
- **Nonlinearities.** Integer-only: polynomial GELU, softmax and LayerNorm in
  the style of I-BERT (arXiv 2101.01321), using fixed integer square-root and
  exponent approximations. No floating-point operations anywhere in the
  graph.
- **Attention.** QKᵀ and AV are activation × activation products. They are
  computed exactly but emit **no tickets**, because consensus cannot pin
  either operand. Only weight matmuls count toward the proof of work: QKV and
  output projections, MLP layers, and the embedding projection.
- **Tokenizer.** The tokenizer and its vocabulary are part of the model spec
  and hashed into `model_id`.
- **Deliverables from the model owner:**
  - the architecture (layer list with shapes);
  - the trained weights;
  - the tokenizer;
  - a license allowing every node to redistribute the weights;
  - an evaluation set to confirm quality after int8 conversion.

`model_id = BLAKE3(spec || tokenizer || weights_root)` is a consensus
parameter. Changing the model is a scheduled upgrade: a new `model_id` at a
height announced in a release.

Construction
------------

### Parameters

| Symbol | Meaning | Candidate | Regtest |
|---|---|---|---|
| `r` | noise rank = tile size (KW: block size equals rank) | 32 | 8 |
| `g` | tiles per K-span of one ticket | 8 | 2 |
| `T` | token rows per batch (padded to a multiple of r) | 4096 | 64 |

These values are provisional. The final ones come from the benchmarks in
"Parameter selection".

### Ring (Variant Z preferred, Variant Q fallback)

Both variants concern **the hardware mining computation**, not the research
market.

- **Z, exact integers.** Noise factors are ternary, {−1, 0, 1}.
  |E|, |F| ≤ r, so A' = A + E and W' = W + F stay in int8, and inference
  results are exact.
  - Security relies on **Assumption A1**, the integer analogue of KW
    Assumption 6.4. It is unproven.
- **Q, arithmetic in F_251.** This is exactly KW.
  - It computes the model only modulo 251, which is useless for inference.
  - It remains as a proof-of-work-only fallback if A1 is broken: mining stays
    secure, but it stops being useful until A1 is repaired.

### Header

The v2 header sets nVersion bit 8 (`VERSION_POWV2`). After the 80 v1 bytes
it adds 40 bytes of fields plus the ticket's activation panel. The block hash
covers all of it, so nothing is malleable:

```
version | prev | merkle_root | time | bits | nonce       (80 bytes, as v1)
batch_root (32)   BLAKE3 commitment to the block's requests (see "Block body")
op         (u16)  index of the weight matmul in the model graph (layer, projection)
tile_i     (u16)  token-row tile          0 <= i < T/r
tile_j     (u16)  output-feature tile     0 <= j < d_out(op)/r
span_s     (u16)  K-span                  0 <= s < d_in(op)/(g*r)
panel      (compact size + r*256 int8)  activation panel of the ticket, |a| <= 95
```

- The header's model is implied by height: the `model_id` active at that
  height.
- The seed is `σ = BLAKE3("ApertureMatMul/v2/seed" || header[0:112])`. It
  commits to the batch before any computation, and excludes only the ticket
  fields.

### Noise, per op

```
X_op,f = BLAKE3-XOF("ApertureMatMul/v2/noise" || σ || op (u16 LE) || f)   f = 0 E_L, 1 E_R, 2 F_L, 3 F_R
entry  = (byte mod 3) - 1                                  one byte per entry, row-major, seekable
E_L (rows×r), E_R (r×d_in), F_L (d_in×r), F_R (r×d_out)
A'     = A_op + E_L·E_R          (activations entering op)
W'     = W_opᵀ + F_L·F_R         (protocol weights of op)
```

Each factor has its own XOF stream. A verifier therefore seeks directly to
the slices of one ticket, without knowing the batch size.

### Tickets

```
P[op][i][j][s] = A'[rows i][K-span s] · W'[K-span s][cols j]     r×r int32, g·r³ MACs
digest         = Fold(P)                                          16 bytes
pow            = BLAKE3(σ || op || i || j || s || digest)
valid          iff pow <= target(nBits)
```

`Fold` is a non-linear, per-entry mixing function that stops linear
sketches from standing in for computing P:

```
y      = (x ^ (x >> 15)) * 0x2c1b3c6d                                  (mod 2^32)
m(x)   = y ^ (y >> 12)
lane_c = Σ_{k ≡ c mod 4} rotl32(m(P_k ^ k), k mod 32)     for c = 0..3   (mod 2^32)
digest = lane_0 || lane_1 || lane_2 || lane_3
```

`Fold` and its constants are provisional and fall under the K0 cryptanalysis
bounty.

A forward pass over T token rows yields
Σ_op (T/r)·(d_out/r)·(d_in/(gr)) tickets.

For example, a 1B-parameter model with about 2e9 weight MACs per token and
T = 4096 performs about 8e12 MACs per pass. That is about 3.2e7 tickets of
g·r³ = 262,144 MACs each.

### Per-layer decode (honest miner)

After each weight matmul, the miner recovers the true product and continues
the forward pass:

```
A_op·W_op = C' − (A_op·F_L)·F_R − E_L·(E_R·W')
```

The overhead is 4r/d_in + 4r/T per op: about 6–8% with r = 32 and
d ≥ 2048. The integer nonlinearity and requantization then produce the next
op's A.

### Verification of a header

The header message carries a proof bundle:

- the activation panel `A[rows i][K-span s]` (g·r² bytes, 8 KiB);
- nothing for the weights, because every node already holds them.

The verifier:

1. reads the weight panel `W_op[K-span s][cols j]` from its local copy of the
   weights, which is checked against `weights_root` once at startup;
2. derives the noise slices for op, i, j and s from σ;
3. computes P, folds it, hashes it, and compares the result with the target.

The cost is about 3·g·r³ ≈ 0.8 M MACs, well under 1 ms. The bundle is about
8 KiB per header, about 2.1 GB per year.

The activation panel is **not** authenticated by the header. It does not need
to be for proof-of-work security, because KW hardness holds for any A. Its
authenticity matters only for embedding correctness, which is handled below.

Block body
----------

As implemented (`src/model/embed.{h,cpp}`):

- **Embedding request.** A transaction output
  `OP_RETURN "APER" <token ids, u24 LE, in pushes of at most 520 bytes>`,
  paid for by the transaction fee.
  - The request is identified by its outpoint.
  - Policy allows one request per transaction, up to 3,100 bytes, exempt
    from the data-carrier limit.
  - Mempool and consensus both reject requests that are too long or use
    token ids outside the vocabulary. The protocol appends EOS.
- **Embedding result.** One coinbase output per request, in block order:
  `OP_RETURN "APEM" <request txid> <vout u32 LE> <int8 embedding in pushes of at most 520 bytes>`,
  with value 0.
- **batch_root** = `BLAKE3("ApertureBatch/v0" || (txid || vout u32 LE)*)`
  over the block's requests, in block order.
- **Consensus checks at block validation** (`ContextualCheckBlock`):
  - at most `nMaxEmbedRequests` (32) requests;
  - the header's batch_root matches;
  - every request can be served;
  - there is exactly one result per request, in order, equal to the
    protocol-model embedding.

  Every validating node recomputes every result ("full verification"). This
  is affordable while the model is small.
- **Empty batch.** With no pending requests, the miner runs the model on the
  EOS-only input. The work is real but unpaid, and it shows up as a low
  useful share.

A miner that finds a ticket partway through the pass finishes the pass (the
remaining layers) and then publishes. **The final embeddings are the block.**

Losing miners have also computed embeddings. Two uses are left open:
- They can deliver them off chain to requesters under payment channels
  (`doc/agent-payments.md`). This is a latency market.
- Or they rejoin the next block's batch, because requests stay pending until
  served on chain.

**The fraud-proof game below replaces full verification** once the protocol
model is too expensive for every node to re-run every request. That is the
case for the placeholder at 440 M MACs per token, and for the project model.

Fraud proofs
------------

Any node can recompute a request's embedding: one forward pass, about 2e9
MACs per token, which takes seconds for typical inputs. On a mismatch:

1. **Challenge.** A transaction that references the result and posts a bond.
2. **Bisection.** The challenger names the first op whose committed output (a
   leaf of act_root) differs from its own. The miner must publish, in a
   response transaction within 72 blocks, that op's input and output rows
   with Merkle paths to act_root.
3. **Single-step check in consensus.** For the named row, nodes recompute one
   op's output row from the committed input row and the local weights. That
   is at most d_in·d_out MACs (16M for 4096 × 4096), or one integer
   nonlinearity step. If the result differs, or the miner does not respond,
   the maturing fees are forfeited: half to the challenger, half to the
   research pool (`doc/research-market.md`).
4. **Failed challenge.** A challenge that fails forfeits its bond to the
   miner.

Because the arithmetic is exact int8/int32, a single-step check is
unambiguous. There is no floating-point tolerance to argue about. This is the
reason for the integer inference profile.

Usefulness accounting
---------------------

- **Useful share** = paid request tokens served ÷ total ticket-eligible tokens
  processed. It is computable from chain data and reported by explorers.
- Miners with no pending requests still run the real model, on junk inputs
  of their choosing. That is visible as a low useful share and earns no fees.
- **Price discovery.** Request fees compete for batch slots like transaction
  fees, and a fee-rate policy orders batches.

Mining protocol
---------------

Stratum V2 (`doc/stratum-v2.md`) extended as follows:

- The Template Provider sends the request batch (inputs and the batch_root) with
  each template.
- The miner runs the forward pass locally with the weights. Weights are
  distributed once, verified against `weights_root`.
- `SubmitSolution.v2` carries the ticket fields, the activation panel, the
  embeddings section and the act_roots.

Parameter selection (testnet gates)
-----------------------------------

1. On an NVIDIA int8 tensor-core GPU, an AMD MFMA GPU, and an
   AVX-512-VNNI/AMX CPU:
   - noisy forward-pass throughput is at least 90% of plain int8 inference of
     the same model;
   - per-op decode overhead is at most 8%.
2. Header verification takes at most 2 ms on one core.
3. Embedding quality under the integer profile is within the model owner's
   stated tolerance of the reference model, on the owner's evaluation set.
4. The fraud-proof game works end to end on testnet: challenge, bisection,
   forfeit and bond.
5. The K0 bounty on A1 and Fold has been open for at least 4 weeks.

Implementation status
---------------------

**Done** (regtest, `-powv2height`):
- `src/crypto/matmulpow_v2`: seed, noise, tickets, Fold, and the KW decode.
- `src/model`: the .apm loader, the integer profile, the tiny model, and
  requests and results.
- The header extension, hashed with the block: batch_root, the ticket fields
  and the activation panel. The block index stores it.
- `ContextualCheckBlockHeader`: the v2 flag must match the height.
- `CheckProofOfWork` over the ticket, using the node's own weights.
- Full verification of the embedding body, and the mempool request check.
- The in-node miner (`generatetoaddress`, `generateblock`), which runs the
  forward pass and searches tickets over the real activations.
- RPCs: `embed`, `createembeddingrequest`, `getblockembeddings`,
  `searchembeddings`, and `getblockheader.powv2` including `pow_hash`.
- `-protocolmodel=<file.apm>` loads another model. Regtest accepts any model
  file.
- **External mining over Stratum V2.**
  - `getblocktemplate` exposes `powv2`.
  - The SV2 useful-work extension (`doc/stratum-v2.md`) carries the batch and
    the solutions.
  - `aperture-sv2-miner` runs the model and mines tickets over its own
    activations.
  - Tested end to end by `feature_sv2_pouw.py`.
- **Tests.**
  - `protocolmodel_tests`: golden vectors shared with Python;
    decode(noisy) = clean; ticket tile = tile of the noisy product.
  - `feature_pouw_v2.py`:
    - activation;
    - a Python/C++ ticket-hash cross-check on real panels;
    - served requests verified by a second node;
    - search;
    - rejection of unservable requests;
    - an independent Python miner;
    - rejection of wrong embeddings, out-of-range panels, and v1 headers after
      activation.

**Not yet:**
- the fraud-proof game and maturing request fees;
- optimized CPU and GPU kernels;
- mainnet and testnet parameters;
- trimming the panel from the in-memory block index (the Zcash-style
  header-on-disk approach).

Implementation plan
-------------------

1. **Integer inference profile.**
   - A reference interpreter in C++ (node) and Python (tests).
   - A converter from the owner's checkpoint to the profile, and a quality
     report.
2. **Tickets.** `src/crypto/matmulpow_v2.{h,cpp}`: noise, tiles, Fold,
   ticket verification using local weights, and cross-language vectors.
3. **Consensus.**
   - The extended header and proof bundle.
   - `model_id` / `weights_root` in chainparams, and a weights store under
     `datadir/model/`.
   - Request outputs, the embeddings section, maturing fee outputs, and
     challenge and response transactions.
4. **Mining.**
   - A regtest in-node miner running a tiny test model.
   - SV2 extensions (done: useful-work extension 0x4150).
   - GPU kernels: CUTLASS-style IMMA and HIP MFMA with a span-boundary
     epilogue, plus CPU VNNI/AMX.
5. **Tests.**
   - Ticket vectors; header sync with bundles; rejection of a weight panel
     that does not match.
   - A served request with correct embeddings.
   - A wrong embedding caught by a fraud proof and forfeited, and a failed
     challenge that forfeits its bond.

Later: training (v3)
--------------------

Miners compute int8 forward and backward passes on training batches. The
gradient matmuls also emit tickets: their operands are activations and
gradients, with the weights still pinned. Weight updates are applied per
epoch through the same fraud-proof game.

This needs three things first:
- integer or deterministic training arithmetic
  ([arXiv:2609.17380](https://arxiv.org/abs/2609.17380) shows bit-exact replay
  is feasible);
- an update rule that is itself fraud-provable;
- a design for the availability of weights and optimizer state.

It is specified separately once v2 inference is stable.

Open items
----------

- A1 and Fold have no proof.
- The weights must be licensed for redistribution by every node.
- Attention products carry no tickets. The ticket-eligible fraction of
  compute falls as sequences get longer.
- Request inputs are public on chain. Confidential embeddings are out of
  scope for v2.
