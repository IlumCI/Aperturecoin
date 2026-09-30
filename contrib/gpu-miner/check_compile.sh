#!/usr/bin/env bash
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Compile the CUDA kernels with clang (no nvcc or GPU needed) for several
# architectures, check that the tensor-core path emits int8 WMMA, and link the
# self-test against libcudart. Usage: check_compile.sh <cuda root> [out dir]
# <cuda root> needs bin/ptxas, include/ (cuda_runtime, curand, cccl) and
# nvvm/libdevice, e.g. assembled from the nvidia-cuda-* Python wheels.
set -euo pipefail
CUDA="$1"
OUT="${2:-$(mktemp -d)}"
mkdir -p "${OUT}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "${HERE}/../../src" && pwd)"
FLAGS=(-std=c++17 -O2 -I"${HERE}/src" -I"${SRC}" -Wno-unknown-cuda-version)
for arch in sm_75 sm_80 sm_86 sm_90; do
  clang++ -x cuda --cuda-path="${CUDA}" --cuda-gpu-arch="${arch}" --cuda-device-only -S "${FLAGS[@]}" \
    "${HERE}/src/aperture_gpu.cu" -o "${OUT}/aperture_gpu.${arch}.ptx"
  clang++ -x cuda --cuda-path="${CUDA}" --cuda-gpu-arch="${arch}" --cuda-device-only -c "${FLAGS[@]}" \
    "${HERE}/src/aperture_gpu.cu" -o "${OUT}/aperture_gpu.${arch}.cubin"
  grep -q "wmma.mma.sync.aligned.row.col.m16n16k16.s32.s8.s8.s32" "${OUT}/aperture_gpu.${arch}.ptx"
  grep -q "dp4a.s32.s32" "${OUT}/aperture_gpu.${arch}.ptx"
  echo "${arch}: cubin OK, int8 WMMA and dp4a present"
done
clang++ -x cuda --cuda-path="${CUDA}" --cuda-gpu-arch=sm_80 -c "${FLAGS[@]}" "${HERE}/src/aperture_gpu.cu" -o "${OUT}/aperture_gpu.o"
gcc -O2 -I"${SRC}" -c "${HERE}/src/blake3_unit.c" -o "${OUT}/blake3_unit.o"
g++ -std=c++17 -O2 -I"${HERE}/src" -I"${SRC}" "${HERE}/src/gpu_selftest.cpp" "${OUT}/aperture_gpu.o" "${OUT}/blake3_unit.o" \
  "${SRC}/crypto/matmulpow_v2.cpp" "${SRC}/crypto/matmulpow_v2_kernel.cpp" \
  -L"${CUDA}/lib64" -Wl,-rpath,"${CUDA}/lib64" -lcudart -lpthread -ldl -lrt -o "${OUT}/gpu_selftest"
echo "linked ${OUT}/gpu_selftest"
