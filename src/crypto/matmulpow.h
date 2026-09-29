// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_MATMULPOW_H
#define BITCOIN_CRYPTO_MATMULPOW_H

#include <stddef.h>
#include <stdint.h>

/**
 * ApertureMatMul v1 proof-of-work (see doc/matmulpow.md).
 *
 *   S      = BLAKE3-XOF("ApertureMatMul/v1" || header, 2*n*n)
 *   A, B   = S[0:n*n], S[n*n:2*n*n] as row-major int8 n x n matrices
 *   C      = A * B with exact int32 accumulation
 *   digest = BLAKE3(C serialized as row-major int32 little-endian)
 *   pow    = BLAKE3(header || digest)
 */
namespace matmulpow {

static constexpr size_t HEADER_LEN = 80;
static constexpr size_t HASH_LEN = 32;
static constexpr unsigned int MAX_DIM = 4096;

/** Returns true if n is a valid matrix dimension (multiple of 16, 16..MAX_DIM). */
bool IsValidDim(unsigned int n);

/** Compute C = A * B for row-major int8 n x n inputs; C must hold n*n entries. */
void MatMulInt8(unsigned int n, const int8_t* a, const int8_t* b, int32_t* c);

/** Compute the 32-byte proof-of-work hash of an 80-byte header. */
void Hash(const unsigned char header[HEADER_LEN], unsigned int n, unsigned char out[HASH_LEN]);

/** Matrix dimension used by CBlockHeader::GetPoWHash(); set by SelectParams(). */
void SetDefaultDim(unsigned int n);
unsigned int GetDefaultDim();

} // namespace matmulpow

#endif // BITCOIN_CRYPTO_MATMULPOW_H
