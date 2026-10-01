ApertureMatMul v2: proof of useful work by protocol-model inference (specification draft)
======================================================================================

Status: implemented. **Testnet runs v2 from block 1** with the placeholder
protocol model (Qwen3-Embedding-0.6B in the integer profile), rank r = 32,
full verification, and a budget of 1,024 request tokens per block.

Why full verification on testnet: a fraud claim carries one layer's input
state, 8 KiB per token for this model, so a claim against a long request does
not fit in a transaction. Optimistic verification needs sub-layer commitments
first (see "Limits and open items" under fraud proofs). Full verification of
one 512-token request takes about 39 s on one core of the reference machine,
which is what the token budget bounds (about 80 s per full block on one core,
less with parallel verification). Regtest activates it with `-powv2height=<n>` and uses the built-in
tiny model. Mainnet stays on v1 until the protocol model and `(r, g)` are
fixed; v2 replaces v1 there before mainnet launches.

Running a testnet node needs the model file once (about 600 MB):

```
pip install -r contrib/aperture-model/requirements.txt
contrib/aperture-model/fetch_protocol_model.py   # installs ~/.aperture/models/<model_id>.apm
apertured -testnet
```

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
| The embeddings in a block are correct | Full verification by default. On optimistic chains, one-step fraud claims forfeit the offending block's coinbase to the challenger ("Fraud proofs"). |
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
  channels**. A ticket's K-block (width r) always lies inside one scale
  group, so every ticket tile is a pure int8 product, and dequantization
  happens after the ticket. The normative definition is in `doc/protocol-model.md`.
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
span_s     (u16)  K-block (width r)       0 <= s < d_in(op)/r
panel      (compact size + r*r int8)    activation panel of the ticket, |a| <= 95
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
P[op][i][j][s] = A'[rows i][K-block s] · W'[K-block s][cols j]   r×r int32, r³ MACs
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
Σ_op (T/r)·(d_out/r)·(d_in/r) tickets.

For example, a 1B-parameter model with about 2e9 weight MACs per token and
T = 4096 performs about 8e12 MACs per pass. That is about 2.4e8 tickets of
r³ = 32,768 MACs each.

**Why a ticket's K-width equals the rank r.** This is KW's Algorithm 6.1,
where the block size equals the rank. An earlier draft used a K-span of
g·r = 256, which is insecure when the miner reuses the same batch across
nonces, as it normally does:
- The miner can precompute the clean tile products A·Wᵀ once.
- Per nonce it then only needs the corrections E_L,i·(E_R,s·Wᵀ_{s,j}) and
  (A'_{i,s}·F_L,s)·F_R,j. Each costs one r×r×r product per ticket after
  work that is amortized over i and j.
- That is about 2r³, against the honest g·r³: a (g/2)-fold shortcut.

With K-width r the corrections cost at least 2r³ against the honest r³, so
the shortcut is slower than honest mining.

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

- the activation panel `A[rows i][K-block s]` (r² bytes, 1 KiB);
- nothing for the weights, because every node already holds them.

The verifier:

1. reads the weight panel `W_op[K-block s][cols j]` from its local copy of the
   weights, which is checked against `weights_root` once at startup;
2. derives the noise slices for op, i, j and s from σ;
3. computes P, folds it, hashes it, and compares the result with the target.

The cost is about 3·r³ ≈ 0.1 M MACs. The reference implementation takes
83 µs on one core. The bundle is about 1 KiB per header, about 0.27 GB per
year.

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

**Fraud proofs (below) replace full verification** on optimistic chains, where
the protocol model is too expensive for every node to re-run every request.
That is the case for the placeholder at 440 M MACs per token, and for the
project model.

Fraud proofs
------------

Implemented: `src/model/embed.cpp`, `CheckFraudClaim` in `validation.cpp`,
and `consensus/fraudclaim.h`. On an **optimistic** chain
(`Consensus::Params::fPowV2Optimistic`; on regtest `-powv2optimistic`):
- Blocks are accepted after checking only the structure of their results:
  one result per request, in order, with L+1 state hashes and an embedding of
  the right size.
- Nodes do not re-run the model.
- A wrong result is proven by anyone, at any time until the offending
  coinbase is spent.

**Commitments.** Every result carries the forward pass's state hashes
`h_0 … h_L`, where L is the number of layers:

```
h_l = BLAKE3("ApertureState/v0" || l (u32 LE) || x_l)      x_l: T x hidden int64 (Q16), after layer l (x_0: embedding lookup)
result = OP_RETURN "APEM" <txid> <vout> <u16 L+1 || h_0 … h_L || embedding int8>
```

**Claim.** The claim is non-interactive. The request, the weights and the
arithmetic are all public and exact, so a challenger recomputes the honest
states and names the **first** step where the miner's commitments diverge:

| step | the claim supplies | nodes re-execute | proven if |
|---|---|---|---|
| 0 | nothing | embedding lookup of the request | `h(x_0) ≠ h_0` |
| l = 1…L | `x_{l-1}`, which must hash to the committed `h_{l-1}` | one layer | `h(layer_l(x_{l-1})) ≠ h_l` |
| L+1 | `x_L`, which must hash to the committed `h_L` | final norm and pooling | `final(x_L) ≠ embedding` |

The miner has no data to withhold: the claim's input state is the honest
state, which the challenger recomputes, and it must match the miner's own
commitment. Verification costs **one step**, not one forward pass.

**Claim transaction**
(`OP_RETURN "APFP" <result index u16> <step u8> <state int64 LE>`, built by
`createfraudclaim`):
- It spends **every positive-value coinbase output of the offending block
  except the development fund**, and pays them, less its fee, wherever the
  challenger chooses.
- It needs no signatures. Script checks, input-standardness checks and
  coinbase maturity are replaced by `CheckFraudClaim`, which runs in both the
  mempool and `ConnectBlock`.
- A claim that proves nothing is simply an invalid transaction, so there is
  no challenger bond.
- A second claim on the same block fails, because the coinbase is already
  spent.

**Challenge window.** The challenge window is the coinbase maturity
(100 blocks). After that the miner can spend the coinbase, and fraud can no
longer be punished. **Cheating on one embedding therefore risks the whole
block reward**, and the reward goes to whoever proves it.

**Monitoring.** `checkblockembeddings <blockhash>` recomputes a block's
results and names the first wrong step of each. A watchtower runs it on
every new block and broadcasts claims.

**Limits and open items:**
- A claim carries one layer's input state, T × hidden × 8 bytes: 128 KiB
  for the tiny model at 64 tokens, 4 MiB for the placeholder at 512 tokens.
  Large models need either sub-layer commitments (per row, or per attention
  and MLP block) or a cap on request length. The current limit is the
  standard transaction weight.
- Anyone can submit claims that make nodes compute one layer before they are
  rejected. The cost is bounded by one step per claim and by the mempool's
  ordinary limits. There is no dedicated rate limit yet.
- The per-block token budget (`nMaxEmbedTokens`, regtest `-powv2maxtokens`)
  bounds the cost of full verification.
- Full verification (`fPowV2Optimistic = false`) is still the default. It also
  checks the state commitments, so a result with a wrong commitment is
  invalid there as well.

Because the arithmetic is exact int8/int32/int64, each step is
unambiguous. There is no floating-point tolerance to argue about, which is
the reason for the integer inference profile.

Usefulness accounting
---------------------

- **Useful share** = weight-matmul work spent on the requests a block served
  ÷ all weight-matmul work spent on the block. `getusefulshare` computes it
  from chain data: the clean forward pass over the block's batch, plus the
  ticket search, estimated as the block's expected tickets (its chainwork
  increment) × r³.

**What the number shows at scale.** A block's batch is computed once
(the clean pass); the ticket search then re-noises the same products for
every nonce. Search work therefore adds proof of work but no new results.
For the placeholder model (440 M MACs per token) and the testnet budget of
1,024 tokens per block, a full block carries 4.5 × 10¹¹ useful MACs. One
AVX-512 VNNI core searches 536 k tickets/s at r = 32, i.e. 1.8 × 10¹⁰ MAC/s,
or 2.1 × 10¹² MACs per 120-s block. The steady-state useful share of full
blocks is therefore about 1 / (1 + 4.7 N) for a network of N such cores:
18% with one core, under 0.01% at a few thousand. This is the same structural
limit as other matmul-PoUW chains: on-chain demand per block is bounded,
hash rate is not.

The candidate fix (not implemented; a consensus change): **bind each attempt
to new work.** Derive the seed from the batch and the previous block only,
with no free nonce and no coinbase-dependent field, so that a batch yields a
fixed number of tickets (its tiles). More tickets then require more batches,
that is, more inference. Hash rate becomes inference throughput, as in the
Komargodski–Weinstein model where miners bring their own matrices. Open
questions: the supply of batches when paid demand is below hash rate (miners
would buy their own requests, so fees set a cost floor on junk work), how
non-winning attempts' results reach their payers (pools, off chain), and
the block-template changes this needs.
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
- **Pooled mining (SRI fork, `contrib/sri-pool`).**
  - The pool runs the forward pass once per template and sends each job's
    batch to miners (SetUsefulWork).
  - Miners submit tickets as TLVs on standard shares.
  - The pool verifies each share with the node's own C++ (`aperture_pouw`
    bindings).
  - Pool policy, stronger than consensus: a share's activation panel must equal
    the pool's forward-pass activations. Consensus leaves A free because KW
    hardness holds for any A. The pool rule makes every pooled ticket work on
    the real inference.
  - Tested end to end by `feature_sri_pool_pouw.py`, including rejection of
    fabricated panels.
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

- **Fraud proofs.** Optimistic verification, per-layer state commitments in
  results, one-step claims that forfeit the coinbase, `checkblockembeddings`
  and `createfraudclaim`.
  - Tested by `feature_pouw_v2_fraud.py` (wrong commitments at steps 0, 1 and
    3; tampered, partial and repeated claims; immature spends).
  - `protocolmodel_tests/fraud_proofs` covers the proof logic.

- **CPU kernels** (`src/crypto/matmulpow_v2_kernel.{h,cpp}`), with runtime
  dispatch between scalar and AVX-512 VNNI.
  - One nonce: ternary noise applied with vector row adds, W′ blocks packed
    for `vpdpbusd` with a +128 offset correction, a vectorized Fold, and
    16-lane BLAKE3 for both the noise stream and the ticket hashes.
  - The integer-profile GEMM runs on the same kernel.
  - `matmulpow_v2_kernel_tests` checks that every ticket of every backend at
    r = 8/16/32 is bit-identical to `TicketPoW`.
  - Used by the in-node miner, `aperture-sv2-miner` (`-kernel=`) and the
    model forward pass.
  - Measured on one core of a 2.8 GHz Xeon (AVX-512 VNNI), for a
    1024 → 1024 op with 64 rows at r = 32:

    | | scalar | AVX-512 VNNI |
    |---|---|---|
    | one nonce (2,048 tickets) | 52.2 ms | 3.8 ms (536k tickets/s, 17.6 GMAC/s of ticket work) |
    | profile GEMM | 6.8 GMAC/s | 32.7 GMAC/s |
    | header ticket verification | 83 µs | (reference path) |

- **GPU kernels** (`contrib/gpu-miner`, CUDA with a HIP compat layer).
  - The whole nonce runs on the device: noise, operands, `dp4a` or WMMA
    int8 tensor-core tiles, Fold, BLAKE3 and the target check.
  - The winning ticket is the same as the CPU's.
  - Wired into `aperture-sv2-miner -kernel=cuda` behind `-DAPERTURE_CUDA=ON`.
  - Compile-verified for sm_75/80/86/90, and the device math is checked on
    the host. They have not run on a GPU yet (see that README).

- **Panel trimming.** Once a block index entry is in the block index
  database, its r × r panel (1 KiB at r = 32) is dropped from memory and read
  back when a header is served (`CBlockIndex::GetPowV2`), as Zcash does with
  Equihash solutions. Rewriting a trimmed entry reloads the panel first.
  Tested by `powv2_index_tests` and `feature_pouw_v2_index.py`.

**Not yet:**
- running the GPU kernels on a GPU (`gpu_selftest`), and compiling the HIP
  path;
- mainnet parameters (testnet: v2 from block 1, r = 32, full verification,
  1,024 tokens per block);
- parallel verification of a block's results, and sub-layer fraud-claim
  commitments so that optimistic verification covers long requests;


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
   - GPU kernels: CUTLASS-style IMMA and HIP MFMA with a K-block-boundary
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
