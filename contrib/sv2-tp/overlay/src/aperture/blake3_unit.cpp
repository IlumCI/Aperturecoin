// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Portable BLAKE3 from the ApertureCoin tree, compiled as one unit.
#define BLAKE3_NO_AVX512 1
#define BLAKE3_NO_AVX2 1
#define BLAKE3_NO_SSE41 1
#define BLAKE3_NO_SSE2 1
extern "C" {
#include <crypto/blake3/blake3.c>
#include <crypto/blake3/blake3_dispatch.c>
#include <crypto/blake3/blake3_portable.c>
}
