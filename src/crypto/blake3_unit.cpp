// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The BLAKE3 reference implementation (src/crypto/blake3), compiled as one
// unit with the portable backend. Used by ApertureMatMul (v1 and v2) and the
// protocol model; the hot ticket hashing has its own 16-way kernels
// (crypto/matmulpow_v2_kernel.cpp).
#define BLAKE3_NO_AVX512 1
#define BLAKE3_NO_AVX2 1
#define BLAKE3_NO_SSE41 1
#define BLAKE3_NO_SSE2 1
extern "C" {
#include <crypto/blake3/blake3.c>
#include <crypto/blake3/blake3_dispatch.c>
#include <crypto/blake3/blake3_portable.c>
}
