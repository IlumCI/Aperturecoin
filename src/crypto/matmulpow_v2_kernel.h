// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_MATMULPOW_V2_KERNEL_H
#define BITCOIN_CRYPTO_MATMULPOW_V2_KERNEL_H

#include <crypto/matmulpow_v2.h>

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <vector>

/**
 * Mining and inference kernels for ApertureMatMul v2 (doc/pouw-v2.md).
 *
 * The reference definitions are in matmulpow_v2.cpp (TicketTile/TicketPoW)
 * and model/intmodel.cpp; every backend here must reproduce them bit for bit
 * (src/test/matmulpow_v2_kernel_tests.cpp).
 *
 * Mining one nonce, per weight matmul (op):
 *   1. ternary noise factors from the seed (BLAKE3 XOF),
 *   2. A' = A + E_L*E_R and W'^T = W + (F_L*F_R)^T with vector adds only,
 *   3. every r x r x r ticket tile with int8 dot products,
 *   4. Fold and the ticket hash (BLAKE3, one block per ticket).
 */
namespace matmulpow_v2 {

enum class Backend {
    SCALAR,       //!< portable C++, always available
    AVX512_VNNI,  //!< x86-64 AVX-512 F/BW + VNNI (vpdpbusd), 16-way BLAKE3
};

/** Fastest backend this CPU supports. */
Backend BestBackend();
const char* BackendName(Backend backend);
bool BackendAvailable(Backend backend);

/** The quantized input of one weight matmul of the forward pass (rows x d_in, |a| <= QMAX). */
struct OpInput {
    uint16_t op{0};
    unsigned int rows{0};
    const int8_t* a{nullptr};
};

struct SearchHit {
    Ticket ticket;
    std::vector<int8_t> panel;  //!< r x r activation panel (A, not A')
    unsigned char pow[32];
};

/**
 * Visit the pow hash of every ticket of one nonce (sigma) in the order
 * op, K-block s, column tile j, row tile i. Rows are zero-padded to a
 * multiple of r. The visitor returns false to stop. Returns the number of
 * tickets visited.
 */
using TicketVisitor = std::function<bool(const Ticket&, const unsigned char pow[32])>;
uint64_t EnumerateTickets(Backend backend, const unsigned char sigma[32], unsigned int r, const std::vector<Op>& ops,
                          const std::vector<OpInput>& inputs, const TicketVisitor& visit);

/**
 * Search one nonce for a ticket with pow <= target (both little-endian
 * uint256). Returns true and fills `hit` on success; `tickets` receives the
 * number of tickets tried.
 */
bool SearchNonce(Backend backend, const unsigned char sigma[32], unsigned int r, const std::vector<Op>& ops,
                 const std::vector<OpInput>& inputs, const unsigned char target[32], SearchHit& hit, uint64_t& tickets);

/**
 * Per-scale-group int8 GEMM of the integer profile:
 *   acc[(t * d_out + o) * (d_in / GROUP) + g] = sum_{k in group g} q[t][k] * w[o][k]
 * q: T x d_in, w: d_out x d_in, |q|, |w| <= 127, d_in a multiple of GROUP.
 */
void GemmGroups(Backend backend, const int8_t* q, const int8_t* w, size_t T, size_t d_in, size_t d_out, int32_t* acc);

/** 16 independent single-block BLAKE3 hashes (exposed for tests). */
void Blake3OneBlock16(Backend backend, const unsigned char* const msgs[16], const size_t lens[16], unsigned char out[16][32]);

} // namespace matmulpow_v2

#endif // BITCOIN_CRYPTO_MATMULPOW_V2_KERNEL_H
