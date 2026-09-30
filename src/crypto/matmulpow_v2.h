// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_MATMULPOW_V2_H
#define BITCOIN_CRYPTO_MATMULPOW_V2_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

/**
 * ApertureMatMul v2: proof of useful work by protocol-model inference
 * (doc/pouw-v2.md, arXiv:2504.09971 Algorithm 6.4 with per-tile tickets).
 *
 * Every weight matmul of the protocol model's forward pass is computed on
 * noised operands A' = A + E_L*E_R and W' = W^T + F_L*F_R (ternary, rank r
 * factors derived from the header seed sigma). Each r x r output tile over a
 * GROUP-wide K-span is a lottery ticket:
 *
 *   P      = A'[rows i][span s] * W'[span s][cols j]        (int32)
 *   digest = Fold(P)                                        (16 bytes)
 *   pow    = BLAKE3(sigma || op || i || j || s || digest)   (LE uint256)
 *
 * A (the int8 activation panel) is supplied by the miner and is not
 * authenticated; W comes from the node's own copy of the pinned weights.
 */
namespace matmulpow_v2 {

static constexpr unsigned int GROUP = 256; //!< K-span = activation scale group (g*r)
static constexpr int QMAX = 95;            //!< |activation| and |weight| bound (127 - 32)
static constexpr unsigned int MAX_RANK = 32;

/** One weight matmul of the protocol model: W is d_out x d_in row-major int8. */
struct Op {
    uint32_t d_in;
    uint32_t d_out;
    const int8_t* w;
};

struct Ticket {
    uint16_t op{0};
    uint16_t i{0};
    uint16_t j{0};
    uint16_t s{0};
};

enum class Factor : uint8_t { EL = 0, ER = 1, FL = 2, FR = 3 };

/** sigma = BLAKE3("ApertureMatMul/v2/seed" || data). */
void Seed(const unsigned char* data, size_t len, unsigned char sigma[32]);

/**
 * Ternary noise bytes: BLAKE3-XOF("ApertureMatMul/v2/noise" || sigma || op
 * (u16 LE) || factor)[offset .. offset+len), each byte b mapped to (b % 3) - 1.
 * Factor layouts (row-major): E_L rows x r, E_R r x d_in, F_L d_in x r,
 * F_R r x d_out.
 */
void Noise(const unsigned char sigma[32], uint16_t op, Factor f, uint64_t offset, size_t len, int8_t* out);

/** Non-linear 16-byte digest of an r x r int32 tile. */
void Fold(const int32_t* p, size_t n, unsigned char digest[16]);

/**
 * Compute the ticket tile P (r x r) for weights `op_desc` and the r x GROUP
 * activation panel. Returns false if the ticket indices or the panel are out
 * of range.
 */
bool TicketTile(const unsigned char sigma[32], unsigned int r, const Op& op_desc, const Ticket& t,
                const int8_t* a_panel, int32_t* p_out);

/** pow = BLAKE3(sigma || op || i || j || s || digest), little-endian. */
void TicketHash(const unsigned char sigma[32], const Ticket& t, const unsigned char digest[16], unsigned char out[32]);

/** TicketTile + Fold + TicketHash. */
bool TicketPoW(const unsigned char sigma[32], unsigned int r, const Op& op_desc, const Ticket& t,
               const int8_t* a_panel, unsigned char out[32]);

/**
 * Honest miner: noisy product of a whole op. a: rows x d_in int8 (rows a
 * multiple of r). Writes A' (rows x d_in) and W' (d_in x d_out, i.e. the
 * noised transpose of W) as int16-free int8-range int32 values.
 */
void NoisyOperands(const unsigned char sigma[32], unsigned int r, uint16_t op, const Op& op_desc,
                   const int8_t* a, unsigned int rows, std::vector<int32_t>& a_noisy, std::vector<int32_t>& w_noisy);

/**
 * Recover the useful per-group product A[:,g] * W^T[g,:] (rows x d_out) from
 * the noisy group product C'_g = A'[:,g] * W'[g,:] (KW Algorithm 6.4 decode):
 *   C_g = C'_g - (A[:,g] F_L[g,:]) F_R - E_L (E_R[:,g] W'[g,:])
 */
void DecodeGroup(const unsigned char sigma[32], unsigned int r, uint16_t op, const Op& op_desc,
                 const int8_t* a, unsigned int rows, unsigned int g, const std::vector<int32_t>& w_noisy,
                 const std::vector<int32_t>& c_noisy_g, std::vector<int32_t>& c_out);

/** Global protocol-model ops used by CBlockHeader::GetPoWHash (set at startup). */
void SetModel(std::vector<Op> ops, unsigned int rank);
const std::vector<Op>& GetOps();
unsigned int GetRank();
bool HasModel();

} // namespace matmulpow_v2

#endif // BITCOIN_CRYPTO_MATMULPOW_V2_H
