// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POW_H
#define BITCOIN_POW_H

#include <consensus/params.h>

#include <stdint.h>

class arith_uint256;
class CBlockHeader;
class CBlockIndex;
class uint256;

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params&);

/**
 * ASERT (aserti3-2d) per-block difficulty adjustment.
 * Returns the target for the block after the one that is nHeightDiff blocks
 * and nTimeDiff seconds past the anchor block, whose target is refTarget.
 */
arith_uint256 CalculateASERT(const arith_uint256& refTarget, int64_t nPowTargetSpacing, int64_t nTimeDiff,
                             int64_t nHeightDiff, const arith_uint256& powLimit, int64_t nHalfLife);

/** Check whether a block hash satisfies the proof-of-work requirement specified by nBits */
bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params&);

#endif // BITCOIN_POW_H
