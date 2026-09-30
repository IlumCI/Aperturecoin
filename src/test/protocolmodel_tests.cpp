// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/matmulpow_v2.h>
#include <model/apm.h>
#include <model/embed.h>
#include <model/intmodel.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <memory>

BOOST_FIXTURE_TEST_SUITE(protocolmodel_tests, BasicTestingSetup)

namespace {

// Golden values from contrib/aperture-model/test/test_intprofile.py.
const char* const TINY_WEIGHTS_ROOT = "75ed16ed07b559919dd343b24d72c06a6ae582bb55278a81264012a151e46c17";
const char* const TINY_MODEL_ID = "f4cb7bf674eedb6bec59de9f06510ecd42599ab4a903366c4c22b56e6c2bc31d";
const std::pair<const char*, const char*> TINY_EMBEDDINGS[] = {
    {"", "974e6dd48a44d827fee01618ad45baaacc55bbe712dc9efbaa102d6d5615695d"},
    {"ApertureCoin", "20857c80a0bb06d05653a933ae14062649a95c57017ed190c227791bd34111ed"},
    {"The quick brown fox jumps over the lazy dog.", "de035429b91db700b5161e63431a8406b6d443b20d0ff4bfddf13e34dcea9eae"},
};

std::unique_ptr<intmodel::IntModel> TinyModel()
{
    auto apm_model = std::make_unique<apm::Model>();
    std::string error;
    BOOST_REQUIRE(apm_model->Load(intmodel::BuildTinyModel(1), error));
    auto model = std::make_unique<intmodel::IntModel>();
    BOOST_REQUIRE_MESSAGE(model->Init(std::move(apm_model), error), error);
    return model;
}

} // namespace

BOOST_AUTO_TEST_CASE(integer_ops)
{
    using namespace intmodel;
    BOOST_CHECK_EQUAL(RDiv(5, 2), 3);
    BOOST_CHECK_EQUAL(RDiv(-5, 2), -2);
    BOOST_CHECK_EQUAL(RDiv(-7, 2), -3);
    for (int64_t n : {int64_t{0}, int64_t{1}, int64_t{15}, int64_t{16}, (int64_t{1} << 62) - 1}) {
        const int64_t r = ISqrt(n);
        BOOST_CHECK(r * r <= n && (r + 1) * (r + 1) > n);
    }
    for (int64_t z = -20 * ONE; z <= 0; z += 997) {
        BOOST_CHECK(std::abs(static_cast<double>(IExpNeg(z)) / ONE - std::exp(static_cast<double>(z) / ONE)) < 0.01);
    }
}

BOOST_AUTO_TEST_CASE(tiny_model_golden)
{
    const auto model = TinyModel();
    BOOST_CHECK_EQUAL(model->Apm().WeightsRootHex(), TINY_WEIGHTS_ROOT);
    BOOST_CHECK_EQUAL(model->Apm().ModelIdHex(), TINY_MODEL_ID);
    BOOST_CHECK_EQUAL(model->Ops().size(), 14U);
    for (const auto& [text, golden] : TINY_EMBEDDINGS) {
        const std::vector<int8_t> e = model->Embed(model->TokenizeBytes(text));
        BOOST_REQUIRE_EQUAL(e.size(), 256U);
        BOOST_CHECK_EQUAL(apm::Blake3Hex(reinterpret_cast<const unsigned char*>(e.data()), e.size()), golden);
    }
}

BOOST_AUTO_TEST_CASE(useful_work_recovery)
{
    // The noisy product that mining computes decodes to the exact useful
    // product, and every ticket tile verifiable from a header equals the
    // corresponding tile of that noisy product.
    const auto model = TinyModel();
    std::vector<intmodel::OpTrace> trace;
    model->Embed(model->TokenizeBytes("proof of useful work"), &trace);
    BOOST_REQUIRE_EQUAL(trace.size(), 14U);

    const unsigned int r = 8;
    unsigned char sigma[32];
    matmulpow_v2::Seed(reinterpret_cast<const unsigned char*>("header"), 6, sigma);

    for (const uint16_t opi : {uint16_t{4}, uint16_t{13}}) { // gate_proj (d_in 256), down_proj (d_in 768)
        const intmodel::OpTrace& tr = trace[opi];
        const matmulpow_v2::Op& od = model->Ops()[opi];
        const unsigned int rows = (tr.rows + r - 1) / r * r;
        std::vector<int8_t> a(static_cast<size_t>(rows) * od.d_in, 0);
        std::copy(tr.q.begin(), tr.q.end(), a.begin());

        std::vector<int32_t> an, wn;
        matmulpow_v2::NoisyOperands(sigma, r, opi, od, a.data(), rows, an, wn);
        for (unsigned int g = 0; g < od.d_in / matmulpow_v2::GROUP; ++g) {
            std::vector<int32_t> cn(static_cast<size_t>(rows) * od.d_out, 0);
            for (unsigned int x = 0; x < rows; ++x)
                for (unsigned int c = 0; c < od.d_out; ++c)
                    for (unsigned int k = g * 256; k < (g + 1) * 256; ++k)
                        cn[x * od.d_out + c] += an[static_cast<size_t>(x) * od.d_in + k] * wn[static_cast<size_t>(k) * od.d_out + c];
            std::vector<int32_t> dec;
            matmulpow_v2::DecodeGroup(sigma, r, opi, od, a.data(), rows, g, wn, cn, dec);
            for (unsigned int x = 0; x < rows; ++x) {
                for (unsigned int c = 0; c < od.d_out; ++c) {
                    int32_t clean = 0;
                    for (unsigned int k = g * 256; k < (g + 1) * 256; ++k)
                        clean += int32_t{a[static_cast<size_t>(x) * od.d_in + k]} * od.w[static_cast<size_t>(c) * od.d_in + k];
                    BOOST_REQUIRE_EQUAL(dec[x * od.d_out + c], clean);
                }
            }
            // Ticket (op, i=0, j=1, s=g) from the header-verifiable path.
            matmulpow_v2::Ticket t;
            t.op = opi;
            t.i = 0;
            t.j = 1;
            t.s = static_cast<uint16_t>(g);
            std::vector<int32_t> tile(r * r);
            std::vector<int8_t> panel(r * 256);
            for (unsigned int x = 0; x < r; ++x)
                std::copy(a.begin() + x * od.d_in + g * 256, a.begin() + x * od.d_in + (g + 1) * 256, panel.begin() + x * 256);
            BOOST_REQUIRE(matmulpow_v2::TicketTile(sigma, r, od, t, panel.data(), tile.data()));
            for (unsigned int x = 0; x < r; ++x)
                for (unsigned int c = 0; c < r; ++c)
                    BOOST_CHECK_EQUAL(tile[x * r + c], cn[x * od.d_out + r + c]);
        }
    }
}

BOOST_AUTO_TEST_CASE(ticket_rejects_bad_input)
{
    const auto model = TinyModel();
    const matmulpow_v2::Op& od = model->Ops()[0];
    unsigned char sigma[32] = {0};
    std::vector<int8_t> panel(8 * 256, 0);
    std::vector<int32_t> tile(64);
    matmulpow_v2::Ticket t;
    BOOST_CHECK(matmulpow_v2::TicketTile(sigma, 8, od, t, panel.data(), tile.data()));
    t.s = 1; // d_in 256 has one span
    BOOST_CHECK(!matmulpow_v2::TicketTile(sigma, 8, od, t, panel.data(), tile.data()));
    t.s = 0;
    t.j = od.d_out / 8;
    BOOST_CHECK(!matmulpow_v2::TicketTile(sigma, 8, od, t, panel.data(), tile.data()));
    t.j = 0;
    panel[5] = 96; // above QMAX
    BOOST_CHECK(!matmulpow_v2::TicketTile(sigma, 8, od, t, panel.data(), tile.data()));
}

BOOST_AUTO_TEST_CASE(fraud_proofs)
{
    // One-step fraud proofs over the per-layer state commitments.
    const auto model = TinyModel();
    const std::vector<uint32_t> input = model->TokenizeBytes("fraud proof");
    embed::Result honest;
    honest.embedding = model->Embed(input, nullptr, &honest.states);
    BOOST_REQUIRE_EQUAL(honest.states.size(), 3U);
    embed::FraudProof proof;
    BOOST_CHECK(!embed::BuildFraudProof(*model, input, honest, 0, proof));

    for (uint8_t step = 0; step <= 3; ++step) {
        embed::Result cheat = honest;
        if (step == 3) {
            cheat.embedding[5] ^= 1;
        } else {
            cheat.states[step][0] ^= 1;
        }
        BOOST_REQUIRE(embed::BuildFraudProof(*model, input, cheat, 7, proof));
        BOOST_CHECK_EQUAL(proof.step, step);
        BOOST_CHECK_EQUAL(proof.result_index, 7);
        std::string why;
        BOOST_CHECK(embed::VerifyFraudProof(*model, input, cheat, proof, why));
        // The same proof does not convict the honest result.
        BOOST_CHECK(!embed::VerifyFraudProof(*model, input, honest, proof, why));
        // Round trip through the claim script.
        embed::FraudProof parsed;
        BOOST_REQUIRE(embed::ParseFraudProof(embed::MakeFraudProofScript(proof), parsed));
        BOOST_CHECK(parsed.step == proof.step && parsed.state == proof.state && parsed.result_index == proof.result_index);
        if (!proof.state.empty()) {
            // A state that is not the committed input is refused.
            embed::FraudProof forged = proof;
            forged.state[0] += 1;
            BOOST_CHECK(!embed::VerifyFraudProof(*model, input, cheat, forged, why));
            BOOST_CHECK_EQUAL(why, "state does not match the committed input");
        }
    }
    // Result scripts round trip with their state commitments.
    embed::Result parsed;
    BOOST_REQUIRE(embed::ParseResult(embed::MakeResultScript(honest), parsed));
    BOOST_CHECK(parsed.states == honest.states && parsed.embedding == honest.embedding);
}

BOOST_AUTO_TEST_SUITE_END()
