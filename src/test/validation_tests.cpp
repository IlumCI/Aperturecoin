// Copyright (c) 2014-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <net.h>
#include <signet.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(validation_tests, TestingSetup)

static void TestBlockSubsidyHalvings(const Consensus::Params& consensusParams)
{
    int maxHalvings = 64;
    CAmount nInitialSubsidy = 50 * COIN;

    CAmount nPreviousSubsidy = nInitialSubsidy * 2; // for height == 0
    BOOST_CHECK_EQUAL(nPreviousSubsidy, nInitialSubsidy * 2);
    for (int nHalvings = 0; nHalvings < maxHalvings; nHalvings++) {
        int nHeight = nHalvings * consensusParams.nSubsidyHalvingInterval;
        CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);
        BOOST_CHECK(nSubsidy <= nInitialSubsidy);
        BOOST_CHECK_EQUAL(nSubsidy, nPreviousSubsidy / 2);
        nPreviousSubsidy = nSubsidy;
    }
    BOOST_CHECK_EQUAL(GetBlockSubsidy(maxHalvings * consensusParams.nSubsidyHalvingInterval, consensusParams), 0);
}

static void TestBlockSubsidyHalvings(int nSubsidyHalvingInterval)
{
    Consensus::Params consensusParams;
    consensusParams.nSubsidyHalvingInterval = nSubsidyHalvingInterval;
    TestBlockSubsidyHalvings(consensusParams);
}

BOOST_AUTO_TEST_CASE(block_subsidy_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    TestBlockSubsidyHalvings(chainParams->GetConsensus()); // As in main
    TestBlockSubsidyHalvings(150); // As in regtest
    TestBlockSubsidyHalvings(1000); // Just another interval
}

BOOST_AUTO_TEST_CASE(subsidy_limit_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    CAmount nSum = 0;
    // 1200 divides the mainnet halving interval (1,051,200), so this sum is exact.
    for (int nHeight = 0; nHeight < 64 * 1051200; nHeight += 1200) {
        CAmount nSubsidy = GetBlockSubsidy(nHeight, chainParams->GetConsensus());
        BOOST_CHECK(nSubsidy <= 50 * COIN);
        nSum += nSubsidy * 1200;
        BOOST_CHECK(MoneyRange(nSum));
    }
    BOOST_CHECK_EQUAL(nSum, CAmount{10511999988436800});
}

BOOST_AUTO_TEST_CASE(devfund_amount_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const Consensus::Params& params = chainParams->GetConsensus();
    BOOST_CHECK_EQUAL(GetDevFundAmount(0, params), 0);
    BOOST_CHECK_EQUAL(GetDevFundAmount(1, params), 75 * COIN / 100);
    BOOST_CHECK_EQUAL(GetDevFundAmount(params.nDevFundEndHeight - 1, params), 75 * COIN / 100);
    BOOST_CHECK_EQUAL(GetDevFundAmount(params.nDevFundEndHeight, params), 0);
    CAmount nSum = 0;
    for (int nHeight = 0; nHeight < params.nDevFundEndHeight; nHeight += 1200) {
        nSum += GetDevFundAmount(nHeight == 0 ? 1 : nHeight, params) * 1200;
    }
    // 1.5% of the first halving era: 1,051,200 blocks * 0.75 SCIENCE.
    BOOST_CHECK_EQUAL(nSum, CAmount{788400} * COIN);
}

BOOST_AUTO_TEST_SUITE_END()
