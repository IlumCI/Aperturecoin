// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <algorithm>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);

    // Testnet: if the new block's timestamp is more than two target spacings
    // after its parent, allow a minimum-difficulty block.
    if (params.fPowAllowMinDifficultyBlocks &&
        pblock->GetBlockTime() > pindexLast->GetBlockTime() + 2 * params.nPowTargetSpacing) {
        return bnPowLimit.GetCompact();
    }

    // Block 1 is mined at the minimum difficulty and anchors ASERT for all
    // later blocks, so the genesis timestamp does not affect difficulty.
    if (pindexLast->nHeight == 0) {
        return bnPowLimit.GetCompact();
    }
    const CBlockIndex* pindexAnchor = pindexLast->GetAncestor(1);
    assert(pindexAnchor != nullptr);
    arith_uint256 refTarget;
    refTarget.SetCompact(pindexAnchor->nBits);

    const int64_t nTimeDiff = pindexLast->GetBlockTime() - pindexAnchor->GetBlockTime();
    const int64_t nHeightDiff = pindexLast->nHeight - pindexAnchor->nHeight;
    return CalculateASERT(refTarget, params.nPowTargetSpacing, nTimeDiff, nHeightDiff, bnPowLimit, params.nASERTHalfLife).GetCompact();
}

arith_uint256 CalculateASERT(const arith_uint256& refTarget, int64_t nPowTargetSpacing, int64_t nTimeDiff,
                             int64_t nHeightDiff, const arith_uint256& powLimit, int64_t nHalfLife)
{
    // aserti3-2d (Toomim / Lundeberg), integer-only:
    //   target = refTarget * 2^((nTimeDiff - nPowTargetSpacing * nHeightDiff) / nHalfLife)
    // nTimeDiff and nHeightDiff are measured from the anchor block itself, so
    // the ideal elapsed time for nHeightDiff blocks is
    // nPowTargetSpacing * nHeightDiff.
    assert(refTarget > 0 && refTarget <= powLimit);
    assert(nHeightDiff >= 0);
    assert(nHalfLife > 0);

    // 16.16 fixed-point exponent. The subtraction cannot overflow for any
    // realistic time or height (|values| < 2^47).
    const int64_t exponent = ((nTimeDiff - nPowTargetSpacing * nHeightDiff) * 65536) / nHalfLife;

    // Arithmetic right shift (floor), well defined since C++20 and on all
    // supported compilers.
    int64_t shifts = exponent >> 16;
    const uint16_t frac = uint16_t(exponent);

    // Cubic approximation of 2^x - 1 on [0, 1), scaled by 2^16. Max error
    // is below 0.013%.
    const uint64_t f = frac;
    const uint32_t factor = 65536 + ((uint64_t{195766423245049} * f + uint64_t{971821376} * f * f +
                                      uint64_t{5127} * f * f * f + (uint64_t{1} << 47)) >> 48);

    // factor < 2^17, so refTarget * factor fits in 256 bits only if refTarget
    // has at most 239 bits. Larger (easier) targets are pre-shifted; the
    // dropped low bits are below 2^-222 relative precision.
    const int pre_shift = std::max(0, int(refTarget.bits()) - 239);
    arith_uint256 nextTarget = (refTarget >> pre_shift) * factor;

    shifts += pre_shift - 16;
    if (shifts <= 0) {
        nextTarget >>= -shifts;
    } else {
        const arith_uint256 shifted = nextTarget << shifts;
        if ((shifted >> shifts) != nextTarget) {
            nextTarget = powLimit;
        } else {
            nextTarget = shifted;
        }
    }

    if (nextTarget == 0) {
        nextTarget = arith_uint256(1);
    } else if (nextTarget > powLimit) {
        nextTarget = powLimit;
    }
    return nextTarget;
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
