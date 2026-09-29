// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/matmulpow.h>
#include <primitives/block.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <vector>

BOOST_FIXTURE_TEST_SUITE(matmulpow_tests, BasicTestingSetup)

static std::vector<unsigned char> VectorHeader()
{
    std::vector<unsigned char> header(80);
    for (int i = 0; i < 80; ++i) header[i] = (unsigned char)(i * 7 + 3);
    return header;
}

static std::string PoWHex(const std::vector<unsigned char>& header, unsigned int n)
{
    unsigned char out[32];
    matmulpow::Hash(header.data(), n, out);
    return HexStr(Span<const unsigned char>(out, 32));
}

BOOST_AUTO_TEST_CASE(test_vectors)
{
    // Shared with test/functional/test_framework/aperture_matmulpow.py.
    const auto header = VectorHeader();
    BOOST_CHECK_EQUAL(PoWHex(header, 16), "d7ee1fb0146153fe17204659e4e8874d4e3574dae638e6a8084fec7efb0587a9");
    BOOST_CHECK_EQUAL(PoWHex(header, 32), "e24c46295e05285325ea259f97f9506dcf3322d40f958636cbc7239cc03fff6a");
    BOOST_CHECK_EQUAL(PoWHex(header, 64), "f75725f63275bd521e7f224592fdfd0b1cf14299b6d7fab6ce0ed6d3d93ec79e");
    BOOST_CHECK_EQUAL(PoWHex(header, 512), "3347007498a9035dc1ba2174094efcbc3f80684deb9c1cb54dc9c933fc5f71ba");
}

BOOST_AUTO_TEST_CASE(header_sensitivity)
{
    auto header = VectorHeader();
    const std::string base = PoWHex(header, 32);
    for (int i = 0; i < 80; ++i) {
        header[i] ^= 1;
        BOOST_CHECK(PoWHex(header, 32) != base);
        header[i] ^= 1;
    }
    BOOST_CHECK_EQUAL(PoWHex(header, 32), base);
}

BOOST_AUTO_TEST_CASE(matmul_exact)
{
    const unsigned int n = 16;
    std::vector<int8_t> a(n * n), b(n * n);
    for (unsigned int i = 0; i < n * n; ++i) {
        a[i] = (int8_t)(i % 2 ? -128 : 127);
        b[i] = (int8_t)(i % 3 ? -128 : 127);
    }
    std::vector<int32_t> c(n * n);
    matmulpow::MatMulInt8(n, a.data(), b.data(), c.data());
    for (unsigned int i = 0; i < n; ++i) {
        for (unsigned int j = 0; j < n; ++j) {
            int32_t expect = 0;
            for (unsigned int k = 0; k < n; ++k) expect += int32_t{a[i * n + k]} * int32_t{b[k * n + j]};
            BOOST_CHECK_EQUAL(c[i * n + j], expect);
        }
    }
}

BOOST_AUTO_TEST_CASE(valid_dims)
{
    BOOST_CHECK(!matmulpow::IsValidDim(0));
    BOOST_CHECK(!matmulpow::IsValidDim(8));
    BOOST_CHECK(!matmulpow::IsValidDim(33));
    BOOST_CHECK(matmulpow::IsValidDim(16));
    BOOST_CHECK(matmulpow::IsValidDim(512));
    BOOST_CHECK(matmulpow::IsValidDim(matmulpow::MAX_DIM));
    BOOST_CHECK(!matmulpow::IsValidDim(matmulpow::MAX_DIM + 16));
}

BOOST_AUTO_TEST_CASE(block_header_pow_hash)
{
    // BasicTestingSetup selects mainnet parameters (n = 512).
    BOOST_CHECK_EQUAL(matmulpow::GetDefaultDim(), 512U);
    const auto header_bytes = VectorHeader();
    CBlockHeader header;
    CDataStream ss(header_bytes, SER_NETWORK, PROTOCOL_VERSION);
    ss >> header;

    unsigned char serialized[80];
    header.SerializeHeader(serialized);
    BOOST_CHECK(std::equal(serialized, serialized + 80, header_bytes.begin()));

    // uint256 stores the hash little-endian, matching the raw BLAKE3 output.
    const uint256 pow_hash = header.GetPoWHash();
    BOOST_CHECK_EQUAL(HexStr(pow_hash), PoWHex(header_bytes, 512));
    BOOST_CHECK(header.GetPoWHash() == pow_hash); // cached
    BOOST_CHECK(header.GetUncachedPoWHash(512) == pow_hash);
    BOOST_CHECK(header.GetUncachedPoWHash(32) != pow_hash);
}

BOOST_AUTO_TEST_SUITE_END()
