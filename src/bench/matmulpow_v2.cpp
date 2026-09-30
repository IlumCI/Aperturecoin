// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <crypto/matmulpow_v2.h>
#include <crypto/matmulpow_v2_kernel.h>

#include <random>
#include <vector>

// ApertureMatMul v2 kernels (doc/pouw-v2.md). One "op" of the shape of a
// 1024 -> 1024 projection with 64 activation rows, rank r = 32: one nonce is
// 2 x 32 x 32 = 2048 tickets of r^3 = 32768 MACs (67 M MACs of useful-shape work).

namespace {

struct Fixture {
    static constexpr unsigned int R = 32, ROWS = 64, DIN = 1024, DOUT = 1024;
    std::vector<int8_t> w, a;
    std::vector<matmulpow_v2::Op> ops;
    std::vector<matmulpow_v2::OpInput> inputs;
    unsigned char sigma[32] = {7};
    Fixture() : w(size_t{DOUT} * DIN), a(size_t{ROWS} * DIN)
    {
        std::mt19937 rng(1);
        for (auto& v : w) v = static_cast<int8_t>(static_cast<int>(rng() % 191) - 95);
        for (auto& v : a) v = static_cast<int8_t>(static_cast<int>(rng() % 191) - 95);
        ops.push_back(matmulpow_v2::Op{DIN, DOUT, w.data()});
        inputs.push_back(matmulpow_v2::OpInput{0, ROWS, a.data()});
    }
};

void Nonce(benchmark::Bench& bench, matmulpow_v2::Backend backend)
{
    if (!matmulpow_v2::BackendAvailable(backend)) return;
    Fixture f;
    const uint64_t tickets = uint64_t{Fixture::ROWS / Fixture::R} * (Fixture::DOUT / Fixture::R) * (Fixture::DIN / Fixture::R);
    bench.batch(tickets).unit("ticket").run([&] {
        uint64_t n = matmulpow_v2::EnumerateTickets(backend, f.sigma, Fixture::R, f.ops, f.inputs,
                                                    [](const matmulpow_v2::Ticket&, const unsigned char*) { return true; });
        ankerl::nanobench::doNotOptimizeAway(n);
        ++f.sigma[0];
    });
}

void Gemm(benchmark::Bench& bench, matmulpow_v2::Backend backend)
{
    if (!matmulpow_v2::BackendAvailable(backend)) return;
    Fixture f;
    std::vector<int32_t> acc(size_t{Fixture::ROWS} * Fixture::DOUT * (Fixture::DIN / matmulpow_v2::GROUP));
    bench.batch(uint64_t{Fixture::ROWS} * Fixture::DIN * Fixture::DOUT).unit("MAC").run([&] {
        matmulpow_v2::GemmGroups(backend, f.a.data(), f.w.data(), Fixture::ROWS, Fixture::DIN, Fixture::DOUT, acc.data());
        ankerl::nanobench::doNotOptimizeAway(acc[0]);
    });
}

void VerifyTicket(benchmark::Bench& bench)
{
    Fixture f;
    std::vector<int8_t> panel(Fixture::R * Fixture::R, 3);
    matmulpow_v2::Ticket t;
    t.j = 5;
    t.s = 7;
    unsigned char out[32];
    bench.run([&] {
        matmulpow_v2::TicketPoW(f.sigma, Fixture::R, f.ops[0], t, panel.data(), out);
        ankerl::nanobench::doNotOptimizeAway(out[0]);
    });
}

void MatMulPoWv2NonceScalar(benchmark::Bench& bench) { Nonce(bench, matmulpow_v2::Backend::SCALAR); }
void MatMulPoWv2NonceAvx512Vnni(benchmark::Bench& bench) { Nonce(bench, matmulpow_v2::Backend::AVX512_VNNI); }
void MatMulPoWv2GemmScalar(benchmark::Bench& bench) { Gemm(bench, matmulpow_v2::Backend::SCALAR); }
void MatMulPoWv2GemmAvx512Vnni(benchmark::Bench& bench) { Gemm(bench, matmulpow_v2::Backend::AVX512_VNNI); }
void MatMulPoWv2VerifyTicket(benchmark::Bench& bench) { VerifyTicket(bench); }

} // namespace

BENCHMARK(MatMulPoWv2NonceScalar);
BENCHMARK(MatMulPoWv2NonceAvx512Vnni);
BENCHMARK(MatMulPoWv2GemmScalar);
BENCHMARK(MatMulPoWv2GemmAvx512Vnni);
BENCHMARK(MatMulPoWv2VerifyTicket);
