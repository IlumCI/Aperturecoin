// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/blake3/blake3.h>
#include <crypto/matmulpow_v2.h>
#include <crypto/matmulpow_v2_kernel.h>
#include <model/apm.h>
#include <model/intmodel.h>
#include <random.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <map>

using namespace matmulpow_v2;

BOOST_FIXTURE_TEST_SUITE(matmulpow_v2_kernel_tests, BasicTestingSetup)

namespace {

std::vector<Backend> Backends()
{
    std::vector<Backend> out{Backend::SCALAR};
    if (BackendAvailable(Backend::AVX512_VNNI)) out.push_back(Backend::AVX512_VNNI);
    return out;
}

std::unique_ptr<intmodel::IntModel> Tiny()
{
    auto apm_model = std::make_unique<apm::Model>();
    std::string error;
    BOOST_REQUIRE(apm_model->Load(intmodel::BuildTinyModel(1), error));
    auto model = std::make_unique<intmodel::IntModel>();
    BOOST_REQUIRE(model->Init(std::move(apm_model), error));
    return model;
}

std::vector<int8_t> RandomActivations(FastRandomContext& rng, size_t n)
{
    std::vector<int8_t> a(n);
    for (auto& v : a) v = static_cast<int8_t>(static_cast<int>(rng.randrange(2 * QMAX + 1)) - QMAX);
    return a;
}

} // namespace

BOOST_AUTO_TEST_CASE(blake3_16way)
{
    FastRandomContext rng(true);
    for (Backend b : Backends()) {
        unsigned char data[16][64], out[16][32];
        const unsigned char* msgs[16];
        size_t lens[16];
        for (int k = 0; k < 16; ++k) {
            for (auto& byte : data[k]) byte = rng.randbits(8);
            lens[k] = (k * 5) % 65; // 0..64 bytes
            msgs[k] = data[k];
        }
        Blake3OneBlock16(b, msgs, lens, out);
        for (int k = 0; k < 16; ++k) {
            unsigned char want[32];
            blake3_hasher h;
            blake3_hasher_init(&h);
            blake3_hasher_update(&h, msgs[k], lens[k]);
            blake3_hasher_finalize(&h, want, 32);
            BOOST_CHECK_MESSAGE(memcmp(want, out[k], 32) == 0, BackendName(b) << " lane " << k);
        }
    }
}

BOOST_AUTO_TEST_CASE(tickets_match_reference)
{
    // Every ticket of a nonce, from every backend and rank (8: generic tiles,
    // 16/32: VNNI tiles), equals the header-verification reference TicketPoW.
    const auto model = Tiny();
    FastRandomContext rng(true);
    const std::vector<uint16_t> ops_used{0, 5, 13}; // q_proj (256x256), up_proj (768 out), down_proj (768 in)
    std::map<uint16_t, std::vector<int8_t>> acts;
    const unsigned int rows = 37; // not a multiple of r: exercises zero padding
    for (uint16_t op : ops_used) acts[op] = RandomActivations(rng, static_cast<size_t>(rows) * model->Ops()[op].d_in);
    std::vector<OpInput> inputs;
    for (uint16_t op : ops_used) inputs.push_back(OpInput{op, rows, acts[op].data()});

    for (unsigned int r : {8u, 16u, 32u}) {
        unsigned char sigma[32];
        for (auto& b : sigma) b = rng.randbits(8);
        std::map<std::tuple<uint16_t, uint16_t, uint16_t, uint16_t>, std::array<unsigned char, 32>> first;
        for (Backend b : Backends()) {
            uint64_t n = EnumerateTickets(b, sigma, r, model->Ops(), inputs, [&](const Ticket& t, const unsigned char pow[32]) {
                const auto key = std::make_tuple(t.op, t.i, t.j, t.s);
                std::array<unsigned char, 32> p;
                memcpy(p.data(), pow, 32);
                auto it = first.find(key);
                if (it == first.end()) {
                    first.emplace(key, p);
                } else {
                    BOOST_CHECK_MESSAGE(it->second == p, BackendName(b) << " r=" << r << " differs");
                }
                return true;
            });
            uint64_t expect = 0;
            for (uint16_t op : ops_used) expect += uint64_t{(rows + r - 1) / r} * (model->Ops()[op].d_out / r) * (model->Ops()[op].d_in / r);
            BOOST_CHECK_EQUAL(n, expect);
        }
        // Spot-check against the reference (TicketPoW recomputes noise per ticket).
        int checked = 0;
        for (const auto& [key, pow] : first) {
            if (checked++ % 97 != 0) continue;
            const auto [op, i, j, s] = key;
            Ticket t;
            t.op = op;
            t.i = i;
            t.j = j;
            t.s = s;
            const Op& od = model->Ops()[op];
            std::vector<int8_t> panel(static_cast<size_t>(r) * r, 0);
            for (unsigned int x = 0; x < r; ++x) {
                const unsigned int row = i * r + x;
                if (row < rows) memcpy(panel.data() + x * r, acts[op].data() + static_cast<size_t>(row) * od.d_in + s * r, r);
            }
            unsigned char want[32];
            BOOST_REQUIRE(TicketPoW(sigma, r, od, t, panel.data(), want));
            BOOST_CHECK_MESSAGE(memcmp(want, pow.data(), 32) == 0, "reference mismatch r=" << r << " op=" << op);
        }
    }
}

BOOST_AUTO_TEST_CASE(search_hit_verifies)
{
    const auto model = Tiny();
    FastRandomContext rng(true);
    const std::vector<int8_t> a = RandomActivations(rng, 8 * 256);
    const std::vector<OpInput> inputs{OpInput{0, 8, a.data()}};
    unsigned char sigma[32] = {1};
    unsigned char target[32];
    memset(target, 0xff, 32);
    target[31] = 0x0f; // ~1/16 of tickets win
    for (Backend b : Backends()) {
        SearchHit hit;
        uint64_t tickets = 0;
        BOOST_REQUIRE(SearchNonce(b, sigma, 8, model->Ops(), inputs, target, hit, tickets));
        BOOST_CHECK(tickets >= 1);
        unsigned char pow[32];
        BOOST_REQUIRE(TicketPoW(sigma, 8, model->Ops()[0], hit.ticket, hit.panel.data(), pow));
        BOOST_CHECK(memcmp(pow, hit.pow, 32) == 0);
        BOOST_CHECK(pow[31] <= 0x0f);
    }
}

BOOST_AUTO_TEST_CASE(gemm_groups_match)
{
    FastRandomContext rng(true);
    const size_t T = 5, din = 512, dout = 24, ng = din / GROUP;
    std::vector<int8_t> q(T * din), w(dout * din);
    for (auto& v : q) v = static_cast<int8_t>(static_cast<int>(rng.randrange(255)) - 127);
    for (auto& v : w) v = static_cast<int8_t>(static_cast<int>(rng.randrange(255)) - 127);
    q[0] = -127;
    w[0] = -127;
    std::vector<int32_t> ref(T * dout * ng), got(T * dout * ng);
    GemmGroups(Backend::SCALAR, q.data(), w.data(), T, din, dout, ref.data());
    for (Backend b : Backends()) {
        GemmGroups(b, q.data(), w.data(), T, din, dout, got.data());
        BOOST_CHECK_MESSAGE(got == ref, BackendName(b));
    }
    int32_t direct = 0;
    for (size_t k = 0; k < GROUP; ++k) direct += int32_t{q[k]} * w[k];
    BOOST_CHECK_EQUAL(ref[0], direct);
}

BOOST_AUTO_TEST_SUITE_END()
