ApertureMatMul v2: proof of useful work (specification draft)
==============================================================

Status: draft for review. Nothing here is implemented. v2 replaces v1
(`doc/matmulpow.md`) on every network before any public network launches, so
it needs no activation height. The testnet, which is not yet public, will be
regenerated.

Goal
----

A block's proof of work must be a matrix product that someone outside the
miner actually wants, such as a layer of batched inference or a quantized
training step. Mining must still meet the usual proof-of-work requirements:
permissionless, verifiable in milliseconds, progress-free, and with no
shortcut cheaper than doing the work.

Lessons from the literature
---------------------------

- **Miner-chosen random matrices produce no useful work.** A deployed matmul
  proof-of-useful-work (PoUW) network ran at about 112 MW and did zero useful
  AI computation. Its verifier accepts random matrices by design, and
  statistical tests on the inputs were beaten by adversarial Gaussian sampling
  ([arXiv:2606.04819](https://arxiv.org/abs/2606.04819)). Usefulness cannot be
  proven about the inputs. v2 therefore does not try: it makes useful work
  *as profitable as* useless work and lets the job market decide, so the share
  of useful work can be measured instead of claimed (see "Usefulness
  accounting").
- **Hashing only the output C is not enough.** For A = B = 0 the noisy
  product collapses to E·F, which costs O(n²r). The proof must commit to
  intermediate tile products. The KW construction
  ([arXiv:2504.09971](https://arxiv.org/abs/2504.09971), Algorithms 6.1, 6.2
  and 6.4) does exactly this. Its security holds for *every* A and B, including
  zero matrices (Section 1.1). It rests on Assumption 6.4, which is not
  proven: all intermediate values of a product of two random rank-r matrices
  cannot be computed in time o(n^(ω_r+1)/r).
- **One ticket per tile product (KW Remark 2.1).** Hashing every intermediate
  tile separately, with each one a lottery ticket, keeps memory bounded. It
  also makes the number of tickets proportional to the work done and allows
  verifying one tile at a time.
- **Market design (economics, [arXiv:2606.06700](https://arxiv.org/abs/2606.06700)).**
  The cost of a majority attack stays tied to the block reward even when
  miners earn outside income. Miners with outside rewards tend to concentrate
  their useful tasks in few blocks
  ([arXiv:2505.21685](https://arxiv.org/abs/2505.21685)). Neither result
  requires consensus to reward usefulness directly.

Construction
------------

### Parameters

| Symbol | Meaning | Candidate (main/test) | Regtest |
|---|---|---|---|
| `n` | job dimension (square n×n × n×n per attempt) | 2048 | 64 |
| `r` | noise rank = tile size (KW: block size equals rank) | 32 | 8 |
| `g` | tiles per K-span of one ticket | 8 | 2 |
| `β` | bound on the magnitude of job entries | 127 − r | 127 − r |

These values are provisional. The final ones are chosen from the benchmarks in
"Parameter selection".

### Ring and noise distribution

KW works over a finite field F_q, with uniform noise factors. v1 and the
installed hardware base both work in int8 with exact int32 accumulation. The
two settings conflict:

- **Variant Z (preferred, integer).** Jobs are int8 matrices with
  |a|, |b| ≤ β. The noise factors E_L, F_L (n×r) and E_R, F_R (r×n) have
  entries drawn uniformly from {−1, 0, 1} by BLAKE3-XOF. So
  |E_ij|, |F_ij| ≤ r, A' = A + E and B' = B + F fit in int8, and the
  products are exact over Z.
  - The client gets AB exactly over Z, which is what int8 inference and
    quantized training need.
  - Security relies on **Assumption A1**: the integer, ternary-factor analogue
    of KW Assumption 6.4. It is unproven, because KW's Lemma 6.5 (noise tiles
    marginally uniform) is specific to F_q.
- **Variant Q (fallback, field).** The same scheme over F_q with q = 251.
  Entries are stored centred in [−125, 125], and int8 tensor cores with int32
  accumulation compute F_q products exactly (each tile sum is below 2^31).
  - This is KW exactly, with Lemma 6.5 and Assumption 6.4 applying directly.
  - The client gets AB mod 251. Exact integer products need a CRT over 3–4
    primes, costing 3–4 times the work. So Variant Q is useful mainly for
    workloads that are already modular, such as lattice cryptography and
    coding.

Recommendation: ship Variant Z on testnet with an open cryptanalysis bounty
on A1 (`doc/research-market.md`, family K0). Switch to Variant Q if A1 is
broken before mainnet.

### Header

The v2 header adds a 38-byte extension to the 80 v1 bytes, for 118 bytes in
all:

```
version | prev | merkle_root | time | bits | nonce       (80 bytes, as v1)
job_root   (32)   Merkle root of the job's input panels
tile_i     (u16)  output tile row      0 <= i < n/r
tile_j     (u16)  output tile column   0 <= j < n/r
span_s     (u16)  K-span index         0 <= s < n/(g*r)
```

- The block hash is SHA256d of all 118 bytes.
- The seed is `σ = BLAKE3("ApertureMatMul/v2/seed" || header[0:112])`. It
  covers everything except the ticket index, so every ticket of one attempt
  shares the same noise.

### Job commitment

- A is split into row panels `A[i][s]` (r × gr), and B into column panels
  `B[s][j]` (gr × r).
- `job_root` is a BLAKE3 Merkle tree whose leaves are
  `BLAKE3("A" || i || s || panel)` and `BLAKE3("B" || s || j || panel)`, in
  canonical order.
- A miner may use any job_root: registered client jobs, its own matrices, or
  all zeros. KW security holds for every A and B.

### Noise

```
X     = BLAKE3-XOF("ApertureMatMul/v2/noise" || σ)
E_L, E_R, F_L, F_R = consecutive ternary matrices read from X
                     (2 bits per entry, rejection-sample the value 3)
E = E_L · E_R ,   F = F_L · F_R            (never formed in full by the verifier)
A' = A + E ,      B' = B + F
```

### Tickets

Each ticket is the product of one tile over one K-span:

```
P[i][j][s] = A'[rows i][K-span s] · B'[K-span s][cols j]      (r×r, int32, g·r³ MACs)
digest     = Fold(P)                                           (16 bytes)
pow        = BLAKE3(σ || i || j || s || digest)                read as uint256 LE
valid      iff pow <= target(nBits)
```

An attempt is one noisy product A'B'. It yields (n/r)² · n/(gr) tickets: 32,768
with the candidate parameters.

A GEMM kernel whose CTA tile is r×r with a K-loop of g steps holds each P as
the difference of its accumulator between span boundaries. The ticket
therefore needs no extra multiplications.

`Fold` is non-linear per entry, so a linear sketch of the product
(Freivalds-style u^T·A'·B'·v, which costs O(n²)) cannot stand in for computing
P:

```
y      = (x ^ (x >> 15)) * 0x2c1b3c6d                                  (mod 2^32)
m(x)   = y ^ (y >> 12)
lane_c = Σ_{k ≡ c mod 4} rotl32(m(P_k ^ k), k mod 32)     for c = 0..3   (mod 2^32)
digest = lane_0 || lane_1 || lane_2 || lane_3
```

The mixing constants are provisional. `Fold` falls under the same
cryptanalysis bounty as A1.

### Verification

The header message carries a proof bundle containing:

- the panels `A[i][s]` and `B[s][j]` (2·g·r² bytes, 16 KiB with the candidate
  parameters);
- their Merkle paths to job_root (about 0.6 KiB).

The verifier:

1. checks the Merkle paths;
2. derives σ, and from X the needed slices: E_L rows of tile i (r×r), E_R and
   F_L for span s (r×gr and gr×r), and F_R columns of tile j (r×r);
3. forms E and F only for those two panels (2·g·r³ MACs);
4. computes P, folds it, hashes it, and compares the result with the target.

With the candidate parameters this is about 0.8 M MACs plus 32 KiB of XOF
output: under 1 ms on a single core. Headers are fully verifiable without the
block body, so header-first sync keeps its denial-of-service resistance.

Proof bundles add about 17 KiB per header, roughly 4.5 GB per year at
120-second blocks. Nodes may prune bundles below the assumevalid point and
keep only header hashes.

### Recovering the useful result (honest miner)

The final accumulators give C' = A'B'. Following KW Algorithm 6.4:

```
AB = C' − (A·F_L)·F_R − E_L·(E_R·B')
```

This is four n×n×r products, so the overhead is 4r/n: 6.25% with the
candidate parameters. Because the factors are ternary, the products can also
be computed with additions only.

The miner returns AB to the client. The client checks it with Freivalds'
algorithm at O(n²) cost per check, without trusting the miner.

Why the known shortcuts fail
----------------------------

| Attack | Why it fails (assuming A1/Assumption 6.4) |
|---|---|
| Zero or structured A, B | Tiles of E·F still cost about r³ each: E_L,i · (E_R,s · F_L,s) · F_R,j has three r×r factors, and no ticket can be computed from shared work in less than r³ (KW Section 6.5). |
| Reusing one job across nonces and precomputing AB tiles | Each ticket's corrections, A·F and E·B', are fresh r×r products per σ. This costs at least as much as the honest tile. |
| Linear sketches of P | `Fold` is non-linear per entry. |
| Choosing matrices after seeing σ | job_root is inside the seeded header bytes, so A and B are fixed before σ exists. |
| Grinding job_root | Equivalent to grinding the nonce: each value is a fresh attempt at full cost. |
| Posting fake jobs to look useful | Brings no advantage, because zero matrices mine equally well. Fake registrations only distort the usefulness metric, and they cost fees (see below). |

Usefulness accounting
---------------------

Consensus does not reward usefulness, because usefulness cannot be verified in
consensus. What it enables is **doing useful work at no mining penalty**. A
miner earns the block reward and the client's fee from the same computation.

- **Job market.** Off-chain, over the Stratum V2 extension described below.
  Clients publish job descriptors: shape, job_root, a price per attempt, and a
  data location.
- **Payment.** Per result tile, over a payment channel. The client verifies
  each tile with Freivalds' algorithm before paying for the next one, so the
  most it can lose is one tile's fee. The atomic contract in
  `doc/agent-payments.md` covers single-shot jobs.
- **Registry (optional, on chain).** The output
  `OP_RETURN "APJB" <job_root> <client_pubkey> <price>` makes a job public.
  Explorers report the **useful share**: the fraction of blocks whose
  job_root is registered by a transaction paying at least a minimum fee per
  attempt. Registering fake jobs costs fees and earns nothing, so the metric
  is honest to within vanity spending. It is a measurement, not a claim.
- **Data availability.** This is the client's problem. Miners fetch the
  panels, and a job with unavailable data is simply not mined. Job inputs
  appear on chain only in the one winning panel pair per block, so
  confidential workloads must accept that exposure or use Variant Q with
  masking. Masking is an open item.

Mining protocol
---------------

Stratum V2 (`doc/stratum-v2.md`) is extended with a **Job Distribution**
sub-protocol, message types allocated in the 0x9x extension range:

- `NewUsefulJob`: provider to pool or miner. Carries job_root, the location of
  the panel data, the price, and the provider's payment channel.
- `JobResultTile`: miner to provider.
- `SubmitSolution.v2`: carries tile_i, tile_j, span_s, and the proof bundle.

The Template Provider validates proof bundles with the same code as the node.

Parameter selection (testnet gates)
-----------------------------------

The candidate `(n, r, g)` values must meet all of the following on reference
hardware:

1. Fold, hash and tile extraction cost at most 10% of GEMM time on an NVIDIA
   int8 tensor-core GPU, an AMD MFMA GPU, and an AVX-512-VNNI or AMX CPU.
2. The recovery overhead 4r/n is at most 7%.
3. Header verification takes at most 2 ms on one core, and the proof bundle
   is at most 24 KiB.
4. The CPU/GPU efficiency ratio is published. It is not a target, but it
   decides how the launch is described.
5. The cryptanalysis bounty (K0) has been open for at least 4 weeks on
   testnet with no break of A1 or Fold.

Implementation plan
-------------------

1. **Reference.** `src/crypto/matmulpow_v2.{h,cpp}` with the scalar Variant
   Z/Q kernels, Fold, and the Merkle tree for the job commitment. A matching
   Python reference goes in `test_framework/aperture_matmulpow_v2.py`, with
   cross-language test vectors.
2. **Header.** The extended `CBlockHeader`, the proof bundle in
   `headers2`/`cmpctblock`/`block` messages, and `CheckProofOfWork`
   delegating to the v2 verifier.
3. **Mining and RPC.** `getblocktemplate` and `submitblock` fields, the
   in-node regtest miner, and the SV2 Job Distribution overlay in
   `contrib/sv2-tp`.
4. **Kernels.** CPU VNNI/AMX, CUDA IMMA (CUTLASS-style with a span-boundary
   epilogue), and HIP MFMA. Each must reproduce the reference vectors.
5. **Tests.** Unit vectors, header-only sync with bundles, rejection of each
   malformed field, all-zero jobs, recovery of AB for a random job, and
   Freivalds acceptance.

Open items
----------

- A1 and Fold have no proof. The bounty and testnet time are the only
  evidence available.
- The appendix scheme in KW (pseudorandom Hadamard rotation, which needs no
  decoding) is not compatible with int8, because of entry growth. Revisit if
  fp8 tensor paths become exact enough.
- Masking confidential job data is open.
- Header proof bundles grow the headers chain by about 4.5 GB per year.
