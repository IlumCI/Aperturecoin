// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Multithreaded nonce search for an ApertureMatMul v1 genesis header.
// Usage: mine_genesis <header76_hex> <nbits_hex> <dim> [threads]
// Prints the first nonce (decimal) whose PoW hash meets the target.
// Build: see contrib/genesis/README.md.

#include <crypto/matmulpow.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static bool MeetsTarget(const unsigned char hash[32], const unsigned char target[32])
{
    // Both little-endian 256-bit integers.
    for (int i = 31; i >= 0; --i) {
        if (hash[i] != target[i]) return hash[i] < target[i];
    }
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <header76_hex> <nbits_hex> <dim> [threads]\n", argv[0]);
        return 1;
    }
    const std::string hex = argv[1];
    if (hex.size() != 152) {
        fprintf(stderr, "header prefix must be 76 bytes\n");
        return 1;
    }
    unsigned char prefix[76];
    for (int i = 0; i < 76; ++i) prefix[i] = (unsigned char)strtoul(hex.substr(2 * i, 2).c_str(), nullptr, 16);
    const uint32_t nbits = (uint32_t)strtoul(argv[2], nullptr, 16);
    const unsigned int dim = (unsigned int)atoi(argv[3]);
    const unsigned int threads = argc > 4 ? (unsigned int)atoi(argv[4]) : std::thread::hardware_concurrency();

    unsigned char target[32] = {0};
    const uint32_t mantissa = nbits & 0x007fffff;
    const int size = nbits >> 24;
    for (int i = 0; i < 3; ++i) {
        const int pos = size - 3 + i;
        if (pos >= 0 && pos < 32) target[pos] = (mantissa >> (8 * i)) & 0xff;
    }

    std::atomic<bool> found{false};
    std::atomic<uint32_t> result{0};
    std::vector<std::thread> workers;
    for (unsigned int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            unsigned char header[80];
            memcpy(header, prefix, 76);
            unsigned char hash[32];
            for (uint64_t nonce = t; nonce <= 0xffffffffULL && !found.load(); nonce += threads) {
                for (int i = 0; i < 4; ++i) header[76 + i] = (nonce >> (8 * i)) & 0xff;
                matmulpow::Hash(header, dim, hash);
                if (MeetsTarget(hash, target)) {
                    if (!found.exchange(true)) result = (uint32_t)nonce;
                    return;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    if (!found) return 2;
    printf("%u\n", result.load());
    return 0;
}
