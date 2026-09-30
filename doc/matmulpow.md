ApertureMatMul v1 proof-of-work
===============================

ApertureCoin (SCIENCE) replaces Litecoin's scrypt with a matrix-multiplication
proof-of-work. It is designed for the int8 matrix engines found in current AI
accelerators and CPUs, such as GPU tensor cores, AVX-512-VNNI and AMX-INT8. It
can be mined on both CPUs and GPUs.

Definition
----------

Inputs:

- `header` is the 80-byte serialized block header.
- `n` is the per-network matrix dimension (`Consensus::Params::nMatMulDim`).

| Network                  | n   |
|--------------------------|-----|
| main, testnet, signet    | 512 |
| regtest                  | 32  |

The algorithm:

```
S      = BLAKE3-XOF("ApertureMatMul/v1" || header, 2*n*n bytes)
A      = S[0      : n*n]   as an n x n int8 matrix, row-major, two's complement
B      = S[n*n    : 2*n*n] as an n x n int8 matrix, row-major, two's complement
C      = A * B             exact integer product, int32 entries
digest = BLAKE3(C serialized row-major as int32 little-endian)
pow    = BLAKE3(header || digest)
```

`pow` is read as a little-endian `uint256`, and a block is valid if it is at
most the target encoded in `nBits`. The block identifier is still
double-SHA256 of the header, `CBlockHeader::GetHash()`.

Properties
----------

- **Exact.** Every step is integer arithmetic. Each entry satisfies
  `|C_ij| <= n * 128 * 128`, which is below 2^31 for `n <= 4096`. CPU and GPU
  implementations are therefore bit-identical, with no floating-point
  nondeterminism.
- **Cost.** Each nonce costs one n^3 int8 multiply-accumulate pass: about
  1.3e8 MACs at n = 512. Generating the matrices costs 2n^2 bytes of BLAKE3
  output, and hashing C costs 4n^2 bytes of BLAKE3 input. Both are small next
  to the multiplication.
- **No precomputation across nonces.** A and B are pseudorandom per header. A
  miner therefore cannot reuse a template-level product, and must hash every
  entry of C.
- **Verification.** A node recomputes one product per header. The portable
  scalar kernel takes about 15 ms on a current x86-64 core.

Why v1 does not use low-rank noise
----------------------------------

Komargodski and Weinstein ([arXiv:2504.09971](https://arxiv.org/abs/2504.09971))
turn the product of arbitrary matrices into a proof of useful work. They do
this by adding low-rank noise that depends on the nonce, `E = U*V` and
`F = X*Y` of rank r. However, if A and B are fixed per template and the
transcript hashes only C, a miner can precompute AB once. After that, each
nonce needs only the low-rank corrections `AF + EB + EF`, at a cost of about
O(n^2 r). That shortcut defeats the proof-of-work unless the transcript also
commits to intermediate tile products and r is chosen relative to the tile
size.

Real usefulness also requires A and B to come from committed external
workloads, with data made available. Otherwise miners multiply random
matrices, which is the failure that
[arXiv:2606.04819](https://arxiv.org/abs/2606.04819) documents for a deployed
matmul-PoW network. v1 therefore uses fully pseudorandom per-nonce matrices,
which admit no shortcut. v2 (`doc/pouw-v2.md`) adds KW25 low-rank noise with one
proof-of-work ticket per tile product, and replaces v1 before any public
network launches.

Mining notes
------------

- At n = 512, every partial sum is at most 512 * 128 * 128 = 2^23, which is
  below 2^24. An FP32-accumulating GEMM on int8-valued inputs is therefore
  also exact, so FP32 and TF32-free BF16/FP16-input/FP32-accumulate tensor
  paths are usable.
- The only per-nonce input is the header, so `getblocktemplate` needs no extra
  fields.
- A GPU miner generates A and B with BLAKE3 on the device. It then runs an
  int8 GEMM with int32 accumulation (IMMA/WMMA on NVIDIA, MFMA on AMD) and
  hashes C on the device.
- Implementations must reproduce the test vectors in
  `src/test/matmulpow_tests.cpp`.
