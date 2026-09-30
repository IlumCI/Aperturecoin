// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/matmulpow_v2.h>

#include <crypto/blake3/blake3.h>
#include <crypto/common.h>

#include <string.h>

#include <mutex>

namespace matmulpow_v2 {
namespace {

const char SEED_TAG[] = "ApertureMatMul/v2/seed";
const char NOISE_TAG[] = "ApertureMatMul/v2/noise";

std::mutex g_model_mutex;
std::vector<Op> g_ops;
unsigned int g_rank = 0;

inline uint32_t Rotl32(uint32_t x, unsigned int k) { return k == 0 ? x : (x << k) | (x >> (32 - k)); }

} // namespace

void Seed(const unsigned char* data, size_t len, unsigned char sigma[32])
{
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, SEED_TAG, sizeof(SEED_TAG) - 1);
    blake3_hasher_update(&h, data, len);
    blake3_hasher_finalize(&h, sigma, 32);
}

void Noise(const unsigned char sigma[32], uint16_t op, Factor f, uint64_t offset, size_t len, int8_t* out)
{
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, NOISE_TAG, sizeof(NOISE_TAG) - 1);
    blake3_hasher_update(&h, sigma, 32);
    unsigned char suffix[3];
    WriteLE16(suffix, op);
    suffix[2] = static_cast<unsigned char>(f);
    blake3_hasher_update(&h, suffix, 3);
    blake3_hasher_finalize_seek(&h, offset, reinterpret_cast<uint8_t*>(out), len);
    for (size_t k = 0; k < len; ++k) {
        out[k] = static_cast<int8_t>(static_cast<int>(static_cast<uint8_t>(out[k]) % 3) - 1);
    }
}

void Fold(const int32_t* p, size_t n, unsigned char digest[16])
{
    uint32_t lane[4] = {0, 0, 0, 0};
    for (size_t k = 0; k < n; ++k) {
        const uint32_t x = static_cast<uint32_t>(p[k]) ^ static_cast<uint32_t>(k);
        const uint32_t y = (x ^ (x >> 15)) * 0x2c1b3c6dU;
        const uint32_t m = y ^ (y >> 12);
        lane[k % 4] += Rotl32(m, k % 32);
    }
    for (int c = 0; c < 4; ++c) WriteLE32(digest + 4 * c, lane[c]);
}

bool TicketTile(const unsigned char sigma[32], unsigned int r, const Op& od, const Ticket& t,
                const int8_t* a_panel, int32_t* p_out)
{
    // Ticket K-width equals the noise rank r (KW Algorithm 6.1: block size = rank),
    // so no ticket can be assembled from precomputed clean tiles plus low-rank
    // corrections for less than the honest r^3 (doc/pouw-v2.md).
    if (r == 0 || r > MAX_RANK || od.d_in % r != 0 || od.d_out % r != 0) return false;
    if (static_cast<uint64_t>(t.s) * r >= od.d_in || static_cast<uint64_t>(t.j) * r >= od.d_out) return false;
    for (unsigned int k = 0; k < r * r; ++k) {
        if (a_panel[k] > QMAX || a_panel[k] < -QMAX) return false;
    }
    std::vector<int8_t> el(r * r), er(r * r), fl(r * r), fr(r * r);
    Noise(sigma, t.op, Factor::EL, static_cast<uint64_t>(t.i) * r * r, r * r, el.data());
    Noise(sigma, t.op, Factor::FL, static_cast<uint64_t>(t.s) * r * r, r * r, fl.data());
    for (unsigned int k = 0; k < r; ++k) {
        Noise(sigma, t.op, Factor::ER, static_cast<uint64_t>(k) * od.d_in + static_cast<uint64_t>(t.s) * r, r, er.data() + k * r);
        Noise(sigma, t.op, Factor::FR, static_cast<uint64_t>(k) * od.d_out + static_cast<uint64_t>(t.j) * r, r, fr.data() + k * r);
    }
    // A' = A + E_L[i rows] * E_R[:, block s]    (r x r)
    std::vector<int32_t> ap(r * r), wp(r * r);
    for (unsigned int a = 0; a < r; ++a) {
        for (unsigned int c = 0; c < r; ++c) {
            int32_t e = 0;
            for (unsigned int k = 0; k < r; ++k) e += el[a * r + k] * er[k * r + c];
            ap[a * r + c] = a_panel[a * r + c] + e;
        }
    }
    // W' = W^T[block s rows, j cols] + F_L[block s] * F_R[:, j cols]    (r x r)
    for (unsigned int k = 0; k < r; ++k) {
        for (unsigned int c = 0; c < r; ++c) {
            int32_t f = 0;
            for (unsigned int q = 0; q < r; ++q) f += fl[k * r + q] * fr[q * r + c];
            const uint64_t row = static_cast<uint64_t>(t.j) * r + c;
            const uint64_t col = static_cast<uint64_t>(t.s) * r + k;
            wp[k * r + c] = od.w[row * od.d_in + col] + f;
        }
    }
    for (unsigned int a = 0; a < r; ++a) {
        for (unsigned int c = 0; c < r; ++c) {
            int32_t acc = 0;
            for (unsigned int k = 0; k < r; ++k) acc += ap[a * r + k] * wp[k * r + c];
            p_out[a * r + c] = acc;
        }
    }
    return true;
}

void TicketHash(const unsigned char sigma[32], const Ticket& t, const unsigned char digest[16], unsigned char out[32])
{
    unsigned char idx[8];
    WriteLE16(idx, t.op);
    WriteLE16(idx + 2, t.i);
    WriteLE16(idx + 4, t.j);
    WriteLE16(idx + 6, t.s);
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, sigma, 32);
    blake3_hasher_update(&h, idx, 8);
    blake3_hasher_update(&h, digest, 16);
    blake3_hasher_finalize(&h, out, 32);
}

bool TicketPoW(const unsigned char sigma[32], unsigned int r, const Op& od, const Ticket& t,
               const int8_t* a_panel, unsigned char out[32])
{
    std::vector<int32_t> p(r * r);
    if (!TicketTile(sigma, r, od, t, a_panel, p.data())) return false;
    unsigned char digest[16];
    Fold(p.data(), p.size(), digest);
    TicketHash(sigma, t, digest, out);
    return true;
}

void NoisyOperands(const unsigned char sigma[32], unsigned int r, uint16_t op, const Op& od,
                   const int8_t* a, unsigned int rows, std::vector<int32_t>& a_noisy, std::vector<int32_t>& w_noisy)
{
    const unsigned int din = od.d_in, dout = od.d_out;
    std::vector<int8_t> el(static_cast<size_t>(rows) * r), er(static_cast<size_t>(r) * din),
        fl(static_cast<size_t>(din) * r), fr(static_cast<size_t>(r) * dout);
    Noise(sigma, op, Factor::EL, 0, el.size(), el.data());
    Noise(sigma, op, Factor::ER, 0, er.size(), er.data());
    Noise(sigma, op, Factor::FL, 0, fl.size(), fl.data());
    Noise(sigma, op, Factor::FR, 0, fr.size(), fr.data());
    a_noisy.assign(static_cast<size_t>(rows) * din, 0);
    for (unsigned int x = 0; x < rows; ++x) {
        for (unsigned int c = 0; c < din; ++c) {
            int32_t e = 0;
            for (unsigned int k = 0; k < r; ++k) e += el[x * r + k] * er[static_cast<size_t>(k) * din + c];
            a_noisy[static_cast<size_t>(x) * din + c] = a[static_cast<size_t>(x) * din + c] + e;
        }
    }
    w_noisy.assign(static_cast<size_t>(din) * dout, 0);
    for (unsigned int k = 0; k < din; ++k) {
        for (unsigned int c = 0; c < dout; ++c) {
            int32_t f = 0;
            for (unsigned int q = 0; q < r; ++q) f += fl[static_cast<size_t>(k) * r + q] * fr[static_cast<size_t>(q) * dout + c];
            w_noisy[static_cast<size_t>(k) * dout + c] = od.w[static_cast<size_t>(c) * din + k] + f;
        }
    }
}

void DecodeGroup(const unsigned char sigma[32], unsigned int r, uint16_t op, const Op& od,
                 const int8_t* a, unsigned int rows, unsigned int g, const std::vector<int32_t>& w_noisy,
                 const std::vector<int32_t>& c_noisy_g, std::vector<int32_t>& c_out)
{
    const unsigned int din = od.d_in, dout = od.d_out, G = GROUP, k0 = g * GROUP;
    std::vector<int8_t> el(static_cast<size_t>(rows) * r), er(static_cast<size_t>(r) * din),
        fl(static_cast<size_t>(din) * r), fr(static_cast<size_t>(r) * dout);
    Noise(sigma, op, Factor::EL, 0, el.size(), el.data());
    Noise(sigma, op, Factor::ER, 0, er.size(), er.data());
    Noise(sigma, op, Factor::FL, 0, fl.size(), fl.data());
    Noise(sigma, op, Factor::FR, 0, fr.size(), fr.data());
    // X = A[:, g] * F_L[g, :]  (rows x r);  Y = E_R[:, g] * W'[g, :]  (r x dout)
    std::vector<int64_t> X(static_cast<size_t>(rows) * r, 0), Y(static_cast<size_t>(r) * dout, 0);
    for (unsigned int x = 0; x < rows; ++x)
        for (unsigned int q = 0; q < r; ++q)
            for (unsigned int k = 0; k < G; ++k)
                X[x * r + q] += a[static_cast<size_t>(x) * din + k0 + k] * fl[static_cast<size_t>(k0 + k) * r + q];
    for (unsigned int q = 0; q < r; ++q)
        for (unsigned int c = 0; c < dout; ++c)
            for (unsigned int k = 0; k < G; ++k)
                Y[static_cast<size_t>(q) * dout + c] += er[static_cast<size_t>(q) * din + k0 + k] * w_noisy[static_cast<size_t>(k0 + k) * dout + c];
    c_out.assign(static_cast<size_t>(rows) * dout, 0);
    for (unsigned int x = 0; x < rows; ++x) {
        for (unsigned int c = 0; c < dout; ++c) {
            int64_t v = c_noisy_g[static_cast<size_t>(x) * dout + c];
            for (unsigned int q = 0; q < r; ++q) {
                v -= X[x * r + q] * fr[static_cast<size_t>(q) * dout + c];
                v -= el[x * r + q] * Y[static_cast<size_t>(q) * dout + c];
            }
            c_out[static_cast<size_t>(x) * dout + c] = static_cast<int32_t>(v);
        }
    }
}

void SetModel(std::vector<Op> ops, unsigned int rank)
{
    std::lock_guard<std::mutex> lock(g_model_mutex);
    g_ops = std::move(ops);
    g_rank = rank;
}

const std::vector<Op>& GetOps() { return g_ops; }
unsigned int GetRank() { return g_rank; }
bool HasModel() { return !g_ops.empty() && g_rank != 0; }

} // namespace matmulpow_v2
