aperture-gpu: GPU kernels for ApertureMatMul v2
===============================================

GPU mining for the v2 proof of useful work (`doc/pouw-v2.md`). One nonce runs
entirely on the device, for each weight matmul of the protocol model's forward
pass:

1. **Noise.** BLAKE3 XOF blocks, one thread per 64-byte block, giving the
   ternary factors E_L, E_R, F_L and F_R.
2. **Operands.** A' = A + E_L·E_R and W'ᵀ = W + (F_L·F_R)ᵀ.
3. **Tickets.** Every r×r×r ticket tile, one 32-lane group per ticket:
   - **Portable path** (CUDA sm_61+ and HIP): `dp4a` int8 dot products, any
     r ≤ 32.
   - **Tensor-core path** (CUDA sm_72+, r = 32): WMMA int8 m16n16k16, 2×2
     fragments over two K halves. Operands are staged in shared memory so
     every fragment pointer is 256-bit aligned.
4. **Hash.** Fold with a 32-lane xor-shuffle reduction, then the ticket's
   BLAKE3 hash and the target check (`atomicMin` keeps the lowest winning
   ticket).

Tickets are numbered like the CPU kernels, so `Miner::Search` returns
exactly the ticket that `matmulpow_v2::SearchNonce` returns. Every hit is
re-verified on the host with the consensus reference `TicketPoW` before it
is reported.

Build
-----

```
cmake -S contrib/gpu-miner -B build-gpu          # CUDA, sm_75/80/86/89/90
cmake --build build-gpu
build-gpu/gpu_selftest
```

`-DAPERTURE_GPU_HIP=ON` builds the portable path for AMD GPUs through the HIP
compat macros (`__builtin_amdgcn_sdot4`, 32-wide shuffles).

**Stratum V2 miner.** Configure sv2-tp with `-DAPERTURE_CUDA=ON`
(`contrib/sv2-tp/build.sh` passes extra CMake flags through `CMAKE_ARGS`).
Then run `aperture-sv2-miner -kernel=cuda ...`. With r = 32 it uses the
tensor-core path, otherwise the portable path.

`gpu_selftest`
--------------

1. **Host check** (needs no GPU). The device math (BLAKE3 compression, the
   Fold term with the 32-lane grouping and shuffle reduction, the ticket
   message layout, the noise stream) is `__host__ __device__` code, run on
   the CPU against the consensus reference.
2. **Device check.** Every ticket of a nonce at r = 8, 16 and 32, on both
   tile paths, must be bit-identical to the CPU kernels. `Search` must pick
   the same ticket as the CPU.
3. **Throughput.** Tickets per second and GMAC/s of ticket work for a
   1024 → 1024 op with 256 rows.

Exit codes: 0 passed, 1 mismatch, 77 no GPU (the host check still runs).

Verification status
-------------------

| | Status |
|---|---|
| CUDA device code, sm_75/80/86/90 | Compiles to cubins (clang 18 + CUDA 12.9 headers, `check_compile.sh`). The PTX contains `wmma.mma.sync…m16n16k16.s32.s8.s8.s32` and `dp4a.s32.s32`. |
| Host link, including `aperture-sv2-miner -kernel=cuda` | Links against libcudart. Without a GPU it reports "no GPU device". |
| Device math on the host | `HostCheck` passes. Deliberate bugs in Fold and in the BLAKE3 message permutation are caught. |
| Kernels on a GPU | **Not run yet**: the development container has no GPU. Run `gpu_selftest` on a CUDA machine before mining with it. |
| HIP | **Not compiled yet**: no ROCm toolchain was available. |

`check_compile.sh <cuda-root>` repeats the compile checks without nvcc or a
GPU. The CUDA root can be assembled from the `nvidia-cuda-runtime-cu12`,
`nvidia-cuda-nvcc-cu12`, `nvidia-curand-cu12` and `nvidia-cuda-cccl-cu12`
wheels, plus `fatbinary` from conda-forge's `cuda-nvcc-tools`.
