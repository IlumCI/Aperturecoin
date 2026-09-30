// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_BLOCK_H
#define BITCOIN_PRIMITIVES_BLOCK_H

#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

/** Nodes collect new transactions into a block, hash them into a hash tree,
 * and scan through nonce values to make the block's hash satisfy proof-of-work
 * requirements.  When they solve the proof-of-work, they broadcast the block
 * to everyone and the block is added to the block chain.  The first transaction
 * in the block is a special one that creates a new coin owned by the creator
 * of the block.
 */
/**
 * ApertureMatMul v2 header extension (doc/pouw-v2.md). Present, and covered by
 * the block hash, only when nVersion has VERSION_POWV2 set.
 */
struct PowV2Proof {
    uint256 batch_root;          //!< commitment to the block's embedding requests
    uint16_t op{0};              //!< weight matmul index in the protocol model
    uint16_t tile_i{0};          //!< activation row tile
    uint16_t tile_j{0};          //!< output column tile
    uint16_t span_s{0};          //!< K-block of width r
    std::vector<int8_t> panel;   //!< r x r int8 activation panel of the ticket

    SERIALIZE_METHODS(PowV2Proof, obj) { READWRITE(obj.batch_root, obj.op, obj.tile_i, obj.tile_j, obj.span_s, obj.panel); }

    void SetNull()
    {
        batch_root.SetNull();
        op = tile_i = tile_j = span_s = 0;
        panel.clear();
    }
};

class CBlockHeader
{
public:
    static constexpr int32_t VERSION_POWV2 = 1 << 8;

    // header
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
    PowV2Proof powv2;

    CBlockHeader()
    {
        SetNull();
    }

    SERIALIZE_METHODS(CBlockHeader, obj)
    {
        READWRITE(obj.nVersion, obj.hashPrevBlock, obj.hashMerkleRoot, obj.nTime, obj.nBits, obj.nNonce);
        if (obj.nVersion & VERSION_POWV2) READWRITE(obj.powv2);
    }

    bool IsPowV2() const { return (nVersion & VERSION_POWV2) != 0; }

    /** sigma input: the 80 v1 bytes followed by batch_root (doc/pouw-v2.md). */
    void SerializeSeedInput(unsigned char out[112]) const;

    void SetNull()
    {
        nVersion = 0;
        hashPrevBlock.SetNull();
        hashMerkleRoot.SetNull();
        nTime = 0;
        nBits = 0;
        nNonce = 0;
        powv2.SetNull();
    }

    bool IsNull() const
    {
        return (nBits == 0);
    }

    uint256 GetHash() const;

    /** ApertureMatMul proof-of-work hash (cached; see doc/matmulpow.md). */
    uint256 GetPoWHash() const;

    /** ApertureMatMul proof-of-work hash for matrix dimension dim, bypassing the cache. */
    uint256 GetUncachedPoWHash(unsigned int dim) const;

    /** Write the 80-byte consensus serialization of the header. */
    void SerializeHeader(unsigned char out[80]) const;

    int64_t GetBlockTime() const
    {
        return (int64_t)nTime;
    }
};


class CBlock : public CBlockHeader
{
public:
    // network and disk
    std::vector<CTransactionRef> vtx;

    // memory only
    mutable bool fChecked;

    CBlock()
    {
        SetNull();
    }

    CBlock(const CBlockHeader &header)
    {
        SetNull();
        *(static_cast<CBlockHeader*>(this)) = header;
    }

    SERIALIZE_METHODS(CBlock, obj)
    {
        READWRITEAS(CBlockHeader, obj);
        READWRITE(obj.vtx);
    }

    void SetNull()
    {
        CBlockHeader::SetNull();
        vtx.clear();
        fChecked = false;
    }

    CBlockHeader GetBlockHeader() const
    {
        CBlockHeader block;
        block.nVersion       = nVersion;
        block.hashPrevBlock  = hashPrevBlock;
        block.hashMerkleRoot = hashMerkleRoot;
        block.nTime          = nTime;
        block.nBits          = nBits;
        block.nNonce         = nNonce;
        block.powv2          = powv2;
        return block;
    }

    std::string ToString() const;
};

/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
struct CBlockLocator
{
    std::vector<uint256> vHave;

    CBlockLocator() {}

    explicit CBlockLocator(const std::vector<uint256>& vHaveIn) : vHave(vHaveIn) {}

    SERIALIZE_METHODS(CBlockLocator, obj)
    {
        int nVersion = s.GetVersion();
        if (!(s.GetType() & SER_GETHASH))
            READWRITE(nVersion);
        READWRITE(obj.vHave);
    }

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull() const
    {
        return vHave.empty();
    }
};

#endif // BITCOIN_PRIMITIVES_BLOCK_H
