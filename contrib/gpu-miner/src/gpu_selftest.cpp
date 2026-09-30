// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Validates the GPU kernels against the CPU kernels (which the node's tests
// validate against the consensus reference) and measures throughput.
//
// Exit codes: 0 all checks passed, 1 mismatch or error, 77 no GPU device.

#include "aperture_gpu.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

using namespace matmulpow_v2;

int main()
{
    std::string report;
    const bool host_ok = aperture_gpu::HostCheck(report);
    std::printf("host check: %s\n", report.c_str());
    if (!host_ok) return 1;
    std::string error;
    const auto devices = aperture_gpu::Devices(&error);
    if (devices.empty()) {
        std::printf("no GPU device available (%s): skipped\n", error.empty() ? "none found" : error.c_str());
        return 77;
    }
    for (const auto& d : devices) std::printf("device %d: %s (sm_%d%d)\n", d.index, d.name.c_str(), d.compute_major, d.compute_minor);

    std::mt19937 rng(1);
    auto random_i8 = [&](size_t n, int bound) {
        std::vector<int8_t> v(n);
        for (auto& x : v) x = static_cast<int8_t>(static_cast<int>(rng() % (2 * bound + 1)) - bound);
        return v;
    };
    // Two ops with different shapes; 50 rows (not a multiple of r).
    std::vector<int8_t> w0 = random_i8(512 * 1024, QMAX), w1 = random_i8(768 * 512, QMAX);
    const std::vector<Op> ops{Op{1024, 512, w0.data()}, Op{512, 768, w1.data()}};
    std::vector<int8_t> a0 = random_i8(50 * 1024, QMAX), a1 = random_i8(50 * 512, QMAX);
    const std::vector<OpInput> inputs{OpInput{0, 50, a0.data()}, OpInput{1, 50, a1.data()}};
    int failures = 0;

    for (unsigned r : {8u, 16u, 32u}) {
        unsigned char sigma[32];
        for (auto& b : sigma) b = rng() & 0xff;
        std::vector<std::array<unsigned char, 32>> cpu;
        EnumerateTickets(BestBackend(), sigma, r, ops, inputs, [&](const Ticket&, const unsigned char pow[32]) {
            std::array<unsigned char, 32> h;
            memcpy(h.data(), pow, 32);
            cpu.push_back(h);
            return true;
        });
        aperture_gpu::Miner miner(devices[0].index, r, ops);
        miner.SetInputs(inputs);
        for (auto path : {aperture_gpu::TilePath::PORTABLE, aperture_gpu::TilePath::TENSOR_CORE}) {
            if (path == aperture_gpu::TilePath::TENSOR_CORE && r != 32) continue;
            const auto gpu = miner.Enumerate(sigma, path);
            size_t bad = gpu.size() == cpu.size() ? 0 : 1;
            for (size_t k = 0; k < gpu.size() && k < cpu.size(); ++k) bad += gpu[k] != cpu[k];
            std::printf("r=%2u %-11s tickets %zu: %s\n", r, path == aperture_gpu::TilePath::PORTABLE ? "portable" : "tensor-core",
                        gpu.size(), bad ? "MISMATCH" : "bit-identical to CPU");
            failures += bad != 0;

            unsigned char target[32];
            memset(target, 0xff, 32);
            target[31] = 0x03;
            SearchHit ghit, chit;
            uint64_t gt = 0, ct = 0;
            const bool gf = miner.Search(sigma, target, path, ghit, gt);
            const bool cf = SearchNonce(BestBackend(), sigma, r, ops, inputs, target, chit, ct);
            const bool same = gf == cf && (!gf || (memcmp(ghit.pow, chit.pow, 32) == 0 && gt == ct));
            std::printf("       search: %s\n", same ? "same ticket as CPU" : "DIFFERENT from CPU");
            failures += !same;
        }
    }

    // Throughput: 1024 -> 1024 op, 256 rows, r = 32 (8,192 tickets per nonce).
    std::vector<int8_t> wb = random_i8(1024 * 1024, QMAX), ab = random_i8(256 * 1024, QMAX);
    const std::vector<Op> bops{Op{1024, 1024, wb.data()}};
    aperture_gpu::Miner bench(devices[0].index, 32, bops);
    bench.SetInputs({OpInput{0, 256, ab.data()}});
    unsigned char never[32] = {0};
    for (auto path : {aperture_gpu::TilePath::PORTABLE, aperture_gpu::TilePath::TENSOR_CORE}) {
        unsigned char sigma[32] = {9};
        SearchHit hit;
        uint64_t tickets = 0, total = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int n = 0; n < 50; ++n) {
            ++sigma[0];
            bench.Search(sigma, never, path, hit, tickets);
            total += tickets;
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%-11s %.0f tickets/s, %.1f GMAC/s of ticket work\n", path == aperture_gpu::TilePath::PORTABLE ? "portable" : "tensor-core",
                    total / s, total * 32768.0 / s / 1e9);
    }
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
