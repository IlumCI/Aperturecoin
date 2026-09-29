// Copyright (c) 2015-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

static arith_uint256 ASERT(const Consensus::Params& params, const arith_uint256& ref, int64_t time_diff, int64_t height_diff)
{
    return CalculateASERT(ref, params.nPowTargetSpacing, time_diff, height_diff, UintToArith256(params.powLimit), params.nASERTHalfLife);
}

/* On schedule, ASERT leaves the target unchanged. */
BOOST_AUTO_TEST_CASE(asert_on_schedule)
{
    const auto params = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    arith_uint256 ref;
    ref.SetCompact(0x1d00ffff);
    for (int64_t h : {0, 1, 10, 1000, 1000000}) {
        BOOST_CHECK(ASERT(params, ref, h * params.nPowTargetSpacing, h) == ref);
    }
}

/* Every half-life ahead of (behind) schedule doubles (halves) the target. */
BOOST_AUTO_TEST_CASE(asert_half_life)
{
    const auto params = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    arith_uint256 ref;
    ref.SetCompact(0x1d00ffff);
    const int64_t h = 5000;
    const int64_t on_time = h * params.nPowTargetSpacing;
    BOOST_CHECK(ASERT(params, ref, on_time + params.nASERTHalfLife, h) == ref * 2);
    BOOST_CHECK(ASERT(params, ref, on_time + 3 * params.nASERTHalfLife, h) == ref * 8);
    BOOST_CHECK(ASERT(params, ref, on_time - params.nASERTHalfLife, h) == ref / 2);
    BOOST_CHECK(ASERT(params, ref, on_time - 4 * params.nASERTHalfLife, h) == ref / 16);

    // Half a half-life ahead: 2^0.5 within the approximation error (< 0.013%).
    const arith_uint256 half = ASERT(params, ref, on_time + params.nASERTHalfLife / 2, h);
    const arith_uint256 expect = ref * 92682 / 65536; // floor(sqrt(2) * 65536) = 92681.9
    const arith_uint256 err = half > expect ? half - expect : expect - half;
    BOOST_CHECK(err * 10000 < expect * 2);
}

/* Target is monotonic in elapsed time and clamped to [1, powLimit]. */
BOOST_AUTO_TEST_CASE(asert_monotonic_and_clamped)
{
    const auto params = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    const arith_uint256 pow_limit = UintToArith256(params.powLimit);
    arith_uint256 ref;
    ref.SetCompact(0x1d00ffff);
    arith_uint256 prev{0};
    for (int64_t t = -20 * params.nASERTHalfLife; t <= 20 * params.nASERTHalfLife; t += params.nASERTHalfLife / 7) {
        const arith_uint256 next = ASERT(params, ref, 1000 * params.nPowTargetSpacing + t, 1000);
        BOOST_CHECK(next >= prev);
        BOOST_CHECK(next <= pow_limit);
        prev = next;
    }
    BOOST_CHECK(ASERT(params, pow_limit, 1000 * params.nASERTHalfLife, 0) == pow_limit);
    BOOST_CHECK(ASERT(params, arith_uint256(1), -1000 * params.nASERTHalfLife, 0) == arith_uint256(1));
}

/* Block 1 is mined at powLimit and anchors ASERT for later blocks. */
BOOST_AUTO_TEST_CASE(get_next_work_asert_anchor)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& params = chainParams->GetConsensus();
    const unsigned int limit_bits = UintToArith256(params.powLimit).GetCompact();
    std::vector<CBlockIndex> blocks(100);
    for (int i = 0; i < 100; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1790640000 + i * params.nPowTargetSpacing;
        blocks[i].nBits = limit_bits;
        blocks[i].BuildSkip();
    }
    CBlockHeader next;
    next.nTime = blocks[0].nTime + params.nPowTargetSpacing;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks[0], &next, params), limit_bits);
    // On schedule relative to block 1: unchanged.
    next.nTime = blocks[99].nTime + params.nPowTargetSpacing;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks[99], &next, params), limit_bits);
    // Blocks arriving twice as fast as scheduled raise the difficulty.
    for (int i = 1; i < 100; i++) blocks[i].nTime = blocks[1].nTime + (i - 1) * params.nPowTargetSpacing / 2;
    arith_uint256 target;
    target.SetCompact(GetNextWorkRequired(&blocks[99], &next, params));
    BOOST_CHECK(target < UintToArith256(params.powLimit));
    // The genesis timestamp does not matter.
    blocks[0].nTime = 1000000000;
    BOOST_CHECK(arith_uint256().SetCompact(GetNextWorkRequired(&blocks[99], &next, params)) == target);
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_negative_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    nBits = UintToArith256(consensus.powLimit).GetCompact(true);
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_overflow_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits = ~0x00800000;
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_too_easy_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 nBits_arith = UintToArith256(consensus.powLimit);
    nBits_arith *= 2;
    nBits = nBits_arith.GetCompact();
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_biger_hash_than_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith = UintToArith256(consensus.powLimit);
    nBits = hash_arith.GetCompact();
    hash_arith *= 2; // hash > nBits
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_zero_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith{0};
    nBits = hash_arith.GetCompact();
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p2 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p3 = &blocks[InsecureRandRange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

void sanity_check_chainparams(const ArgsManager& args, std::string chainName)
{
    const auto chainParams = CreateChainParams(args, chainName);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());

    // target timespan is an even multiple of spacing
    BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan % consensus.nPowTargetSpacing, 0);

    // genesis nBits is positive, doesn't overflow and is lower than powLimit
    arith_uint256 pow_compact;
    bool neg, over;
    pow_compact.SetCompact(chainParams->GenesisBlock().nBits, &neg, &over);
    BOOST_CHECK(!neg && pow_compact != 0);
    BOOST_CHECK(!over);
    BOOST_CHECK(UintToArith256(consensus.powLimit) >= pow_compact);

    // ASERT at powLimit must not overflow in either direction
    if (!consensus.fPowNoRetargeting) {
        const arith_uint256 pow_limit = UintToArith256(consensus.powLimit);
        BOOST_CHECK(consensus.nASERTHalfLife > 0);
        // Compare in compact form: pre-shifting large targets drops low bits
        // that nBits cannot represent anyway.
        BOOST_CHECK_EQUAL(CalculateASERT(pow_limit, consensus.nPowTargetSpacing, 0, 0, pow_limit, consensus.nASERTHalfLife).GetCompact(), pow_limit.GetCompact());
        BOOST_CHECK(CalculateASERT(pow_limit, consensus.nPowTargetSpacing, 50 * consensus.nASERTHalfLife, 0, pow_limit, consensus.nASERTHalfLife) == pow_limit);
        BOOST_CHECK_EQUAL(CalculateASERT(pow_limit, consensus.nPowTargetSpacing, -consensus.nASERTHalfLife, 0, pow_limit, consensus.nASERTHalfLife).GetCompact(), arith_uint256(pow_limit / 2).GetCompact());
    }

    // genesis satisfies its own proof of work
    BOOST_CHECK(CheckProofOfWork(chainParams->GenesisBlock().GetUncachedPoWHash(consensus.nMatMulDim),
                                 chainParams->GenesisBlock().nBits, consensus));
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_REGTEST_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::TESTNET);
}

BOOST_AUTO_TEST_CASE(ChainParams_SIGNET_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::SIGNET);
}

BOOST_AUTO_TEST_SUITE_END()
