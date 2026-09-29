// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/matmulpow.h>

#include <crypto/blake3/blake3.h>
#include <crypto/common.h>

#include <assert.h>
#include <atomic>
#include <string.h>
#include <vector>

namespace matmulpow {
namespace {

const char TAG[] = "ApertureMatMul/v1";
std::atomic<unsigned int> g_default_dim{512};

} // namespace

bool IsValidDim(unsigned int n)
{
    return n >= 16 && n <= MAX_DIM && n % 16 == 0;
}

void MatMulInt8(unsigned int n, const int8_t* a, const int8_t* b, int32_t* c)
{
    // i-k-j order: the inner loop is a contiguous int8 -> int32 multiply-add
    // that compilers vectorize. Integer arithmetic makes results exact and
    // identical across CPU and GPU implementations: |c| <= n * 128 * 128.
    memset(c, 0, sizeof(int32_t) * n * n);
    for (unsigned int i = 0; i < n; ++i) {
        int32_t* crow = c + (size_t)i * n;
        const int8_t* arow = a + (size_t)i * n;
        for (unsigned int k = 0; k < n; ++k) {
            const int32_t aik = arow[k];
            const int8_t* brow = b + (size_t)k * n;
            for (unsigned int j = 0; j < n; ++j) {
                crow[j] += aik * brow[j];
            }
        }
    }
}

void Hash(const unsigned char header[HEADER_LEN], unsigned int n, unsigned char out[HASH_LEN])
{
    assert(IsValidDim(n));
    const size_t nn = (size_t)n * n;
    std::vector<int8_t> ab(2 * nn);
    std::vector<int32_t> c(nn);

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, TAG, sizeof(TAG) - 1);
    blake3_hasher_update(&hasher, header, HEADER_LEN);
    blake3_hasher_finalize(&hasher, reinterpret_cast<uint8_t*>(ab.data()), ab.size());

    MatMulInt8(n, ab.data(), ab.data() + nn, c.data());

    // Serialize C as little-endian int32, row-major, and hash it.
    blake3_hasher_init(&hasher);
    unsigned char buf[4096];
    size_t pos = 0;
    for (size_t i = 0; i < nn; ++i) {
        WriteLE32(buf + pos, (uint32_t)c[i]);
        pos += 4;
        if (pos == sizeof(buf)) {
            blake3_hasher_update(&hasher, buf, pos);
            pos = 0;
        }
    }
    if (pos) blake3_hasher_update(&hasher, buf, pos);
    unsigned char digest[HASH_LEN];
    blake3_hasher_finalize(&hasher, digest, HASH_LEN);

    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, header, HEADER_LEN);
    blake3_hasher_update(&hasher, digest, HASH_LEN);
    blake3_hasher_finalize(&hasher, out, HASH_LEN);
}

void SetDefaultDim(unsigned int n)
{
    assert(IsValidDim(n));
    g_default_dim.store(n, std::memory_order_relaxed);
}

unsigned int GetDefaultDim()
{
    return g_default_dim.load(std::memory_order_relaxed);
}

} // namespace matmulpow
