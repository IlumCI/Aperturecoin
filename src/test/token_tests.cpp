// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/token.h>
#include <script/interpreter.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(token_tests, BasicTestingSetup)

static uint256 Category()
{
    return uint256S("0102030405060708091011121314151617181920212223242526272829303132");
}

static CScript P2WPKH()
{
    return CScript() << OP_0 << std::vector<unsigned char>(20, 0x42);
}

BOOST_AUTO_TEST_CASE(encode_parse_roundtrip)
{
    const std::vector<token::TokenData> cases{
        {Category(), std::nullopt, 1},
        {Category(), std::nullopt, std::numeric_limits<int64_t>::max()},
        {Category(), token::NFT{token::Capability::NONE, {}}, 0},
        {Category(), token::NFT{token::Capability::MUTABLE, {0xaa}}, 0},
        {Category(), token::NFT{token::Capability::MINTING, std::vector<unsigned char>(40, 0x01)}, 253},
    };
    for (const auto& td : cases) {
        BOOST_REQUIRE(token::IsValid(td));
        const CScript spk{token::Encode(td, P2WPKH())};
        BOOST_CHECK(token::HasTokenPrefix(spk));
        token::TokenData parsed;
        CScript locking;
        BOOST_REQUIRE(token::Parse(spk, parsed, &locking) == token::ParseResult::OK);
        BOOST_CHECK(parsed == td);
        BOOST_CHECK(locking == P2WPKH());
        BOOST_CHECK(token::GetLockingBytecode(spk) == P2WPKH());
    }
}

BOOST_AUTO_TEST_CASE(known_encoding)
{
    // Fungible amount 1000 (CompactSize fd e803) and a mutable NFT with commitment "ab".
    token::TokenData td{Category(), token::NFT{token::Capability::MUTABLE, {0xab}}, 1000};
    const CScript spk{token::Encode(td, CScript() << OP_TRUE)};
    const std::string expected{"ef" + HexStr(Category()) + "71" + "01ab" + "fde803" + "51"};
    BOOST_CHECK_EQUAL(HexStr(spk), expected);
}

BOOST_AUTO_TEST_CASE(invalid_prefixes)
{
    const std::string cat{HexStr(Category())};
    const auto parse = [](const std::string& hex) {
        const auto bytes{ParseHex(hex)};
        token::TokenData td;
        return token::Parse(CScript(bytes.begin(), bytes.end()), td);
    };
    BOOST_CHECK(parse("51") == token::ParseResult::NO_TOKEN);
    BOOST_CHECK(parse("ef") == token::ParseResult::INVALID);                   // truncated
    BOOST_CHECK(parse("ef" + cat + "00") == token::ParseResult::INVALID);      // no nft, no amount
    BOOST_CHECK(parse("ef" + cat + "90" + "01") == token::ParseResult::INVALID); // reserved bit
    BOOST_CHECK(parse("ef" + cat + "13" + "01") == token::ParseResult::INVALID); // capability without nft
    BOOST_CHECK(parse("ef" + cat + "23") == token::ParseResult::INVALID);      // capability 3
    BOOST_CHECK(parse("ef" + cat + "60" + "00") == token::ParseResult::INVALID); // empty commitment
    BOOST_CHECK(parse("ef" + cat + "60" + "29" + std::string(82, '0')) == token::ParseResult::INVALID); // 41 bytes
    BOOST_CHECK(parse("ef" + cat + "10" + "00") == token::ParseResult::INVALID); // amount 0
    BOOST_CHECK(parse("ef" + cat + "10" + "fd0100") == token::ParseResult::INVALID); // non-canonical CompactSize
    BOOST_CHECK(parse("ef" + cat + "10" + "ff0000000000000080") == token::ParseResult::INVALID); // > INT64_MAX
    BOOST_CHECK(parse("ef" + cat + "10" + "fd") == token::ParseResult::INVALID); // truncated amount
    BOOST_CHECK(parse("ef" + cat + "20") == token::ParseResult::OK);           // immutable NFT, empty commitment
}

BOOST_AUTO_TEST_CASE(solver_and_verify_strip_prefix)
{
    token::TokenData td{Category(), std::nullopt, 5};
    const CScript spk{token::Encode(td, P2WPKH())};
    std::vector<std::vector<unsigned char>> solutions;
    BOOST_CHECK(Solver(spk, solutions) == TxoutType::WITNESS_V0_KEYHASH);
    CTxDestination dest;
    BOOST_CHECK(ExtractDestination(spk, dest));
    BOOST_CHECK(dest == CTxDestination(WitnessV0KeyHash(uint160(std::vector<unsigned char>(20, 0x42)))));

    // OP_TRUE behind a token prefix is spendable with an empty scriptSig.
    const CScript anyone{token::Encode(td, CScript() << OP_TRUE)};
    ScriptError err;
    BOOST_CHECK(VerifyScript(CScript(), anyone, nullptr, SCRIPT_VERIFY_P2SH, BaseSignatureChecker(), &err));
    // An invalid prefix is never spendable.
    std::vector<unsigned char> bad{token::PREFIX_TOKEN};
    BOOST_CHECK(!VerifyScript(CScript(), CScript(bad.begin(), bad.end()), nullptr, SCRIPT_VERIFY_P2SH, BaseSignatureChecker(), &err));
}

BOOST_AUTO_TEST_CASE(taproot_precompute_sees_through_prefix)
{
    // Regression: a token-carrying P2TR output must trigger the BIP341
    // precomputation, otherwise a Schnorr signature check would assert.
    token::TokenData td{Category(), std::nullopt, 5};
    const CScript p2tr{CScript() << OP_1 << std::vector<unsigned char>(32, 0x02)};
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptWitness.stack.push_back(std::vector<unsigned char>(64, 0x01));
    tx.vout.emplace_back(1000, CScript() << OP_TRUE);
    PrecomputedTransactionData txdata;
    txdata.Init(tx, {CTxOut(2000, token::Encode(td, p2tr))});
    BOOST_CHECK(txdata.m_bip341_taproot_ready);
}

BOOST_AUTO_TEST_SUITE_END()
