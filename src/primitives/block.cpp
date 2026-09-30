// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <hash.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <crypto/common.h>
#include <crypto/matmulpow.h>
#include <crypto/matmulpow_v2.h>

#include <mutex>
#include <unordered_map>

uint256 CBlockHeader::GetHash() const
{
    return SerializeHash(*this);
}

namespace {

struct PoWCacheEntry {
    unsigned int dim;
    uint256 pow_hash;
};

struct BlockHashHasher {
    size_t operator()(const uint256& hash) const { return ReadLE64(hash.begin()); }
};

/**
 * ApertureMatMul hashes cost milliseconds each, and the same header is hashed
 * repeatedly (header and block validation, reads from disk, reorgs). Cache
 * results keyed by the double-SHA256 block hash, which commits to the same
 * 80 bytes.
 */
constexpr size_t POW_CACHE_MAX_ENTRIES = 1 << 15;
std::mutex g_pow_cache_mutex;
std::unordered_map<uint256, PoWCacheEntry, BlockHashHasher> g_pow_cache;

} // namespace

void CBlockHeader::SerializeHeader(unsigned char out[80]) const
{
    WriteLE32(out, (uint32_t)nVersion);
    memcpy(out + 4, hashPrevBlock.begin(), 32);
    memcpy(out + 36, hashMerkleRoot.begin(), 32);
    WriteLE32(out + 68, nTime);
    WriteLE32(out + 72, nBits);
    WriteLE32(out + 76, nNonce);
}

void CBlockHeader::SerializeSeedInput(unsigned char out[112]) const
{
    SerializeHeader(out);
    memcpy(out + 80, powv2.batch_root.begin(), 32);
}

uint256 CBlockHeader::GetPoWHash() const
{
    const unsigned int dim = IsPowV2() ? 0 : matmulpow::GetDefaultDim();
    const uint256 block_hash = GetHash();
    {
        std::lock_guard<std::mutex> lock(g_pow_cache_mutex);
        const auto it = g_pow_cache.find(block_hash);
        if (it != g_pow_cache.end() && it->second.dim == dim) return it->second.pow_hash;
    }
    uint256 pow_hash = GetUncachedPoWHash(dim);
    {
        std::lock_guard<std::mutex> lock(g_pow_cache_mutex);
        if (g_pow_cache.size() >= POW_CACHE_MAX_ENTRIES) g_pow_cache.clear();
        g_pow_cache[block_hash] = PoWCacheEntry{dim, pow_hash};
    }
    return pow_hash;
}

uint256 CBlockHeader::GetUncachedPoWHash(unsigned int dim) const
{
    if (IsPowV2()) {
        // Invalid tickets map to the maximum hash, which fails every target.
        uint256 pow_hash{uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")};
        const unsigned int r = matmulpow_v2::GetRank();
        const std::vector<matmulpow_v2::Op>& ops = matmulpow_v2::GetOps();
        if (!matmulpow_v2::HasModel() || powv2.op >= ops.size() || powv2.panel.size() != r * r) return pow_hash;
        unsigned char seed_input[112], sigma[32];
        SerializeSeedInput(seed_input);
        matmulpow_v2::Seed(seed_input, sizeof(seed_input), sigma);
        matmulpow_v2::Ticket t;
        t.op = powv2.op;
        t.i = powv2.tile_i;
        t.j = powv2.tile_j;
        t.s = powv2.span_s;
        uint256 out;
        if (matmulpow_v2::TicketPoW(sigma, r, ops[t.op], t, powv2.panel.data(), out.begin())) pow_hash = out;
        return pow_hash;
    }
    unsigned char header[80];
    SerializeHeader(header);
    uint256 pow_hash;
    matmulpow::Hash(header, dim, pow_hash.begin());
    return pow_hash;
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}

CTransactionRef CBlock::GetHogEx() const noexcept
{
    if (vtx.size() >= 2 && vtx.back()->IsHogEx()) {
        assert(!vtx.back()->vout.empty());
        return vtx.back();
    }

    return nullptr;
}