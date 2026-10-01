// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/matmulpow_v2_kernel.h>

#include <crypto/blake3/blake3.h>
#include <crypto/common.h>

#include <string.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <vector>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define APERTURE_X86_KERNELS 1
#include <immintrin.h>
#endif

namespace matmulpow_v2 {
namespace {

const char NOISE_TAG[] = "ApertureMatMul/v2/noise";
constexpr uint32_t IV[8] = {0x6A09E667UL, 0xBB67AE85UL, 0x3C6EF372UL, 0xA54FF53AUL,
                            0x510E527FUL, 0x9B05688CUL, 0x1F83D9ABUL, 0x5BE0CD19UL};
constexpr uint8_t MSG_SCHEDULE[7][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13},
};
constexpr uint32_t CHUNK_START = 1, CHUNK_END = 2, ROOT = 8;
constexpr uint32_t FOLD_MUL = 0x2c1b3c6dU;

struct TernaryTable {
    int8_t v[256];
    TernaryTable()
    {
        for (int b = 0; b < 256; ++b) v[b] = static_cast<int8_t>(b % 3 - 1);
    }
};
const TernaryTable TERNARY;

//! Noise stream message: tag || sigma || op (u16 LE) || factor (one BLAKE3 block).
size_t NoiseMessage(const unsigned char sigma[32], uint16_t op, Factor f, unsigned char msg[64])
{
    const size_t tag = sizeof(NOISE_TAG) - 1;
    memset(msg, 0, 64);
    memcpy(msg, NOISE_TAG, tag);
    memcpy(msg + tag, sigma, 32);
    WriteLE16(msg + tag + 32, op);
    msg[tag + 34] = static_cast<unsigned char>(f);
    return tag + 35;
}

size_t TicketMessage(const unsigned char sigma[32], const Ticket& t, const unsigned char digest[16], unsigned char msg[64])
{
    memset(msg, 0, 64);
    memcpy(msg, sigma, 32);
    WriteLE16(msg + 32, t.op);
    WriteLE16(msg + 34, t.i);
    WriteLE16(msg + 36, t.j);
    WriteLE16(msg + 38, t.s);
    memcpy(msg + 40, digest, 16);
    return 56;
}

uint32_t Rotl(uint32_t x, unsigned k) { return k == 0 ? x : (x << k) | (x >> (32 - k)); }

void FoldScalar(const int32_t* p, size_t n, unsigned char digest[16])
{
    uint32_t lane[4] = {0, 0, 0, 0};
    for (size_t k = 0; k < n; ++k) {
        const uint32_t x = static_cast<uint32_t>(p[k]) ^ static_cast<uint32_t>(k);
        const uint32_t y = (x ^ (x >> 15)) * FOLD_MUL;
        lane[k % 4] += Rotl(y ^ (y >> 12), k % 32);
    }
    for (int c = 0; c < 4; ++c) WriteLE32(digest + 4 * c, lane[c]);
}

/** Per-op noisy operands of one nonce: A' (rows_p x d_in) and W'^T (d_out x d_in). */
struct NoisyOp {
    unsigned int rows_p{0};
    std::vector<int8_t> a;   //!< A'
    std::vector<int8_t> wt;  //!< W'^T
};

/** dst[k] += sign * src[k] (int8, wrapping; results stay in range by construction). */
inline __attribute__((always_inline)) void RowAddScalar(int8_t* __restrict dst, const int8_t* __restrict src, size_t n, int sign)
{
    if (sign > 0) {
        for (size_t k = 0; k < n; ++k) dst[k] = static_cast<int8_t>(dst[k] + src[k]);
    } else {
        for (size_t k = 0; k < n; ++k) dst[k] = static_cast<int8_t>(dst[k] - src[k]);
    }
}

template <typename NoiseFn, typename RowAdd>
inline __attribute__((always_inline)) void BuildNoisyOp(unsigned int r, const Op& od, const OpInput& in, NoiseFn&& noise, RowAdd&& row_add, NoisyOp& out)
{
    const unsigned int din = od.d_in, dout = od.d_out, rows_p = (in.rows + r - 1) / r * r;
    out.rows_p = rows_p;
    std::vector<int8_t> el(static_cast<size_t>(rows_p) * r), er(static_cast<size_t>(r) * din),
        fl(static_cast<size_t>(din) * r), fr(static_cast<size_t>(r) * dout);
    noise(Factor::EL, el);
    noise(Factor::ER, er);
    noise(Factor::FL, fl);
    noise(Factor::FR, fr);
    // A' = A + E_L E_R: ternary coefficients, so only row adds and subtracts.
    out.a.assign(static_cast<size_t>(rows_p) * din, 0);
    for (unsigned int x = 0; x < rows_p; ++x) {
        int8_t* dst = out.a.data() + static_cast<size_t>(x) * din;
        if (x < in.rows) memcpy(dst, in.a + static_cast<size_t>(x) * din, din);
        for (unsigned int k = 0; k < r; ++k) {
            const int8_t e = el[static_cast<size_t>(x) * r + k];
            if (e != 0) row_add(dst, er.data() + static_cast<size_t>(k) * din, din, e);
        }
    }
    // W'^T[c][k] = W[c][k] + sum_q F_R[q][c] F_L[k][q]
    std::vector<int8_t> flt(static_cast<size_t>(r) * din);
    for (unsigned int k = 0; k < din; ++k)
        for (unsigned int q = 0; q < r; ++q) flt[static_cast<size_t>(q) * din + k] = fl[static_cast<size_t>(k) * r + q];
    out.wt.assign(od.w, od.w + static_cast<size_t>(dout) * din);
    for (unsigned int c = 0; c < dout; ++c) {
        int8_t* dst = out.wt.data() + static_cast<size_t>(c) * din;
        for (unsigned int q = 0; q < r; ++q) {
            const int8_t f = fr[static_cast<size_t>(q) * dout + c];
            if (f != 0) row_add(dst, flt.data() + static_cast<size_t>(q) * din, din, f);
        }
    }
}

inline __attribute__((always_inline)) void TileGeneric(const NoisyOp& no, unsigned int din, unsigned int r, unsigned int i, unsigned int j, unsigned int s, int32_t* p)
{
    for (unsigned int a = 0; a < r; ++a) {
        const int8_t* ar = no.a.data() + static_cast<size_t>(i * r + a) * din + s * r;
        for (unsigned int c = 0; c < r; ++c) {
            const int8_t* wr = no.wt.data() + static_cast<size_t>(j * r + c) * din + s * r;
            int32_t acc = 0;
            for (unsigned int k = 0; k < r; ++k) acc += int32_t{ar[k]} * wr[k];
            p[a * r + c] = acc;
        }
    }
}

/** Collects up to 16 tickets and hashes them in one batch. */
struct HashBatch {
    std::array<Ticket, 16> tickets;
    std::array<std::array<unsigned char, 64>, 16> msgs;
    size_t n{0};
};

// ------------------------------------------------------------- scalar --

void NoiseScalar(const unsigned char sigma[32], uint16_t op, Factor f, std::vector<int8_t>& out)
{
    unsigned char msg[64];
    const size_t len = NoiseMessage(sigma, op, f, msg);
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, msg, len);
    blake3_hasher_finalize_seek(&h, 0, reinterpret_cast<uint8_t*>(out.data()), out.size());
    for (auto& v : out) v = TERNARY.v[static_cast<uint8_t>(v)];
}

void HashOneScalar(const unsigned char* msg, size_t len, unsigned char out[32])
{
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, msg, len);
    blake3_hasher_finalize(&h, out, 32);
}

uint64_t EnumerateScalar(const unsigned char sigma[32], unsigned int r, const std::vector<Op>& ops,
                         const std::vector<OpInput>& inputs, const TicketVisitor& visit)
{
    uint64_t count = 0;
    std::vector<int32_t> p(static_cast<size_t>(r) * r);
    for (const OpInput& in : inputs) {
        const Op& od = ops.at(in.op);
        NoisyOp no;
        BuildNoisyOp(r, od, in, [&](Factor f, std::vector<int8_t>& v) { NoiseScalar(sigma, in.op, f, v); }, RowAddScalar, no);
        for (unsigned int s = 0; s < od.d_in / r; ++s) {
            for (unsigned int j = 0; j < od.d_out / r; ++j) {
                for (unsigned int i = 0; i < no.rows_p / r; ++i) {
                    TileGeneric(no, od.d_in, r, i, j, s, p.data());
                    Ticket t;
                    t.op = in.op;
                    t.i = static_cast<uint16_t>(i);
                    t.j = static_cast<uint16_t>(j);
                    t.s = static_cast<uint16_t>(s);
                    unsigned char digest[16], msg[64], pow[32];
                    FoldScalar(p.data(), p.size(), digest);
                    HashOneScalar(msg, TicketMessage(sigma, t, digest, msg), pow);
                    ++count;
                    if (!visit(t, pow)) return count;
                }
            }
        }
    }
    return count;
}

void GemmGroupsScalar(const int8_t* q, const int8_t* w, size_t T, size_t din, size_t dout, int32_t* acc)
{
    const size_t ng = din / GROUP;
    for (size_t t = 0; t < T; ++t)
        for (size_t o = 0; o < dout; ++o)
            for (size_t g = 0; g < ng; ++g) {
                int32_t sum = 0;
                for (size_t k = g * GROUP; k < (g + 1) * GROUP; ++k) sum += int32_t{q[t * din + k]} * w[o * din + k];
                acc[(t * dout + o) * ng + g] = sum;
            }
}

#ifdef APERTURE_X86_KERNELS
// --------------------------------------------------------- AVX-512 VNNI --
#define AVX512_TARGET __attribute__((target("avx512f,avx512bw,avx512vl,avx512vnni")))

/** 16 BLAKE3 compressions (IV chaining value, one root block each); v[16] out. */
AVX512_TARGET void Compress16(const __m512i m[16], __m512i counter_lo, __m512i counter_hi, __m512i block_len, uint32_t flags, __m512i v[16])
{
    for (int k = 0; k < 8; ++k) v[k] = _mm512_set1_epi32(static_cast<int>(IV[k]));
    for (int k = 0; k < 4; ++k) v[8 + k] = _mm512_set1_epi32(static_cast<int>(IV[k]));
    v[12] = counter_lo;
    v[13] = counter_hi;
    v[14] = block_len;
    v[15] = _mm512_set1_epi32(static_cast<int>(flags));
#define APERTURE_G(a, b, c, d, mx, my)                                    \
    do {                                                                  \
        v[a] = _mm512_add_epi32(_mm512_add_epi32(v[a], v[b]), mx);        \
        v[d] = _mm512_ror_epi32(_mm512_xor_si512(v[d], v[a]), 16);        \
        v[c] = _mm512_add_epi32(v[c], v[d]);                              \
        v[b] = _mm512_ror_epi32(_mm512_xor_si512(v[b], v[c]), 12);        \
        v[a] = _mm512_add_epi32(_mm512_add_epi32(v[a], v[b]), my);        \
        v[d] = _mm512_ror_epi32(_mm512_xor_si512(v[d], v[a]), 8);         \
        v[c] = _mm512_add_epi32(v[c], v[d]);                              \
        v[b] = _mm512_ror_epi32(_mm512_xor_si512(v[b], v[c]), 7);         \
    } while (0)
    for (int round = 0; round < 7; ++round) {
        const uint8_t* s = MSG_SCHEDULE[round];
        APERTURE_G(0, 4, 8, 12, m[s[0]], m[s[1]]);
        APERTURE_G(1, 5, 9, 13, m[s[2]], m[s[3]]);
        APERTURE_G(2, 6, 10, 14, m[s[4]], m[s[5]]);
        APERTURE_G(3, 7, 11, 15, m[s[6]], m[s[7]]);
        APERTURE_G(0, 5, 10, 15, m[s[8]], m[s[9]]);
        APERTURE_G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        APERTURE_G(2, 7, 8, 13, m[s[12]], m[s[13]]);
        APERTURE_G(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
#undef APERTURE_G
}

/** Load 16 64-byte messages as 16 word vectors (lane = message). */
AVX512_TARGET void LoadMessages(const unsigned char* const msgs[16], __m512i m[16])
{
    alignas(64) uint32_t w[16][16];
    for (int lane = 0; lane < 16; ++lane)
        for (int k = 0; k < 16; ++k) w[k][lane] = ReadLE32(msgs[lane] + 4 * k);
    for (int k = 0; k < 16; ++k) m[k] = _mm512_load_si512(w[k]);
}

AVX512_TARGET void Hash16(const unsigned char* const msgs[16], const uint32_t lens[16], unsigned char out[16][32])
{
    __m512i m[16], v[16];
    LoadMessages(msgs, m);
    const __m512i len = _mm512_loadu_si512(lens);
    Compress16(m, _mm512_setzero_si512(), _mm512_setzero_si512(), len, CHUNK_START | CHUNK_END | ROOT, v);
    alignas(64) uint32_t h[8][16];
    for (int k = 0; k < 8; ++k) _mm512_store_si512(h[k], _mm512_xor_si512(v[k], v[k + 8]));
    for (int lane = 0; lane < 16; ++lane)
        for (int k = 0; k < 8; ++k) WriteLE32(out[lane] + 4 * k, h[k][lane]);
}

/** Noise stream: 16 XOF output blocks per compression batch. */
AVX512_TARGET void NoiseAvx512(const unsigned char sigma[32], uint16_t op, Factor f, std::vector<int8_t>& out)
{
    unsigned char msg[64];
    const uint32_t len = static_cast<uint32_t>(NoiseMessage(sigma, op, f, msg));
    __m512i m[16];
    for (int k = 0; k < 16; ++k) m[k] = _mm512_set1_epi32(static_cast<int>(ReadLE32(msg + 4 * k)));
    const __m512i iota = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    const size_t blocks = (out.size() + 63) / 64;
    alignas(64) uint32_t words[16][16];
    unsigned char buf[16 * 64];
    for (size_t b0 = 0; b0 < blocks; b0 += 16) {
        __m512i v[16];
        const __m512i ctr = _mm512_add_epi32(_mm512_set1_epi32(static_cast<int>(b0 & 0xffffffff)), iota);
        Compress16(m, ctr, _mm512_set1_epi32(static_cast<int>(b0 >> 32)), _mm512_set1_epi32(static_cast<int>(len)),
                   CHUNK_START | CHUNK_END | ROOT, v);
        for (int k = 0; k < 8; ++k) {
            _mm512_store_si512(words[k], _mm512_xor_si512(v[k], v[k + 8]));
            _mm512_store_si512(words[k + 8], _mm512_xor_si512(v[k + 8], _mm512_set1_epi32(static_cast<int>(IV[k]))));
        }
        for (int lane = 0; lane < 16; ++lane)
            for (int k = 0; k < 16; ++k) WriteLE32(buf + 64 * lane + 4 * k, words[k][lane]);
        const size_t off = b0 * 64, n = std::min<size_t>(sizeof(buf), out.size() - off);
        for (size_t k = 0; k < n; ++k) out[off + k] = TERNARY.v[buf[k]];
    }
}

AVX512_TARGET void FoldAvx512(const int32_t* p, size_t n, unsigned char digest[16])
{
    const __m512i iota = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    const __m512i mul = _mm512_set1_epi32(static_cast<int>(FOLD_MUL));
    __m512i lanes = _mm512_setzero_si512();
    for (size_t base = 0; base < n; base += 16) {
        const __m512i idx = _mm512_add_epi32(_mm512_set1_epi32(static_cast<int>(base)), iota);
        const __m512i x = _mm512_xor_si512(_mm512_loadu_si512(p + base), idx);
        const __m512i y = _mm512_mullo_epi32(_mm512_xor_si512(x, _mm512_srli_epi32(x, 15)), mul);
        const __m512i mm = _mm512_xor_si512(y, _mm512_srli_epi32(y, 12));
        lanes = _mm512_add_epi32(lanes, _mm512_rolv_epi32(mm, _mm512_and_si512(idx, _mm512_set1_epi32(31))));
    }
    alignas(64) uint32_t l[16];
    _mm512_store_si512(l, lanes);
    for (int c = 0; c < 4; ++c) WriteLE32(digest + 4 * c, l[c] + l[c + 4] + l[c + 8] + l[c + 12]);
}

/**
 * VNNI tile for r % 16 == 0: A' is offset to u8 (+128, stored as au) and the
 * W' block is packed [k/4][c][4]; P = sum (a'+128) w' - 128 * colsum(w').
 */
AVX512_TARGET void TileVnni(const uint8_t* au, unsigned int din, unsigned int r, unsigned int i, unsigned int s,
                            const int8_t* pack, const int32_t* colsum, int32_t* p)
{
    const unsigned int nv = r / 16, kq = r / 4;
    __m512i corr[2];
    for (unsigned int v = 0; v < nv; ++v) corr[v] = _mm512_slli_epi32(_mm512_loadu_si512(colsum + 16 * v), 7);
    for (unsigned int a = 0; a < r; ++a) {
        const uint8_t* row = au + static_cast<size_t>(i * r + a) * din + s * r;
        __m512i acc[2] = {_mm512_setzero_si512(), _mm512_setzero_si512()};
        for (unsigned int q = 0; q < kq; ++q) {
            int32_t quad;
            memcpy(&quad, row + 4 * q, 4);
            const __m512i b = _mm512_set1_epi32(quad);
            for (unsigned int v = 0; v < nv; ++v) acc[v] = _mm512_dpbusd_epi32(acc[v], b, _mm512_loadu_si512(pack + (static_cast<size_t>(q) * r + 16 * v) * 4));
        }
        for (unsigned int v = 0; v < nv; ++v) _mm512_storeu_si512(p + a * r + 16 * v, _mm512_sub_epi32(acc[v], corr[v]));
    }
}

AVX512_TARGET void RowAddAvx512(int8_t* __restrict dst, const int8_t* __restrict src, size_t n, int sign)
{
    size_t k = 0;
    if (sign > 0) {
        for (; k + 64 <= n; k += 64) _mm512_storeu_si512(dst + k, _mm512_add_epi8(_mm512_loadu_si512(dst + k), _mm512_loadu_si512(src + k)));
    } else {
        for (; k + 64 <= n; k += 64) _mm512_storeu_si512(dst + k, _mm512_sub_epi8(_mm512_loadu_si512(dst + k), _mm512_loadu_si512(src + k)));
    }
    RowAddScalar(dst + k, src + k, n - k, sign);
}

AVX512_TARGET void TileGenericAvx512(const NoisyOp& no, unsigned int din, unsigned int r, unsigned int i, unsigned int j, unsigned int s, int32_t* p)
{
    TileGeneric(no, din, r, i, j, s, p);
}

AVX512_TARGET uint64_t EnumerateAvx512(const unsigned char sigma[32], unsigned int r, const std::vector<Op>& ops,
                                       const std::vector<OpInput>& inputs, const TicketVisitor& visit)
{
    uint64_t count = 0;
    std::vector<int32_t> p(static_cast<size_t>(r) * r);
    HashBatch batch;
    bool stop = false;
    auto flush = [&]() {
        if (batch.n == 0) return;
        const unsigned char* msgs[16];
        uint32_t lens[16];
        unsigned char out[16][32];
        for (size_t k = 0; k < 16; ++k) {
            msgs[k] = batch.msgs[k < batch.n ? k : 0].data();
            lens[k] = 56;
        }
        Hash16(msgs, lens, out);
        for (size_t k = 0; k < batch.n && !stop; ++k) {
            ++count;
            if (!visit(batch.tickets[k], out[k])) stop = true;
        }
        batch.n = 0;
    };
    const bool vnni = r % 16 == 0 && r <= 32;
    std::vector<int8_t> pack;
    std::vector<int32_t> colsum(r);
    for (const OpInput& in : inputs) {
        const Op& od = ops.at(in.op);
        NoisyOp no;
        BuildNoisyOp(r, od, in, [&](Factor f, std::vector<int8_t>& v) { NoiseAvx512(sigma, in.op, f, v); }, RowAddAvx512, no);
        std::vector<uint8_t> au;
        if (vnni) {
            au.resize(no.a.size());
            for (size_t k = 0; k < au.size(); ++k) au[k] = static_cast<uint8_t>(no.a[k]) ^ 0x80;
            pack.resize(static_cast<size_t>(r) * r);
        }
        for (unsigned int s = 0; s < od.d_in / r && !stop; ++s) {
            for (unsigned int j = 0; j < od.d_out / r && !stop; ++j) {
                if (vnni) {
                    // Pack the W' block as [k/4][c][4] (4-byte groups move whole).
                    for (unsigned int c = 0; c < r; ++c) {
                        const int8_t* wr = no.wt.data() + static_cast<size_t>(j * r + c) * od.d_in + s * r;
                        int32_t sum = 0;
                        for (unsigned int q = 0; q < r / 4; ++q) {
                            memcpy(pack.data() + (static_cast<size_t>(q) * r + c) * 4, wr + 4 * q, 4);
                            sum += wr[4 * q] + wr[4 * q + 1] + wr[4 * q + 2] + wr[4 * q + 3];
                        }
                        colsum[c] = sum;
                    }
                }
                for (unsigned int i = 0; i < no.rows_p / r && !stop; ++i) {
                    if (vnni) {
                        TileVnni(au.data(), od.d_in, r, i, s, pack.data(), colsum.data(), p.data());
                    } else {
                        TileGenericAvx512(no, od.d_in, r, i, j, s, p.data());
                    }
                    Ticket& t = batch.tickets[batch.n];
                    t.op = in.op;
                    t.i = static_cast<uint16_t>(i);
                    t.j = static_cast<uint16_t>(j);
                    t.s = static_cast<uint16_t>(s);
                    unsigned char digest[16];
                    FoldAvx512(p.data(), p.size(), digest);
                    TicketMessage(sigma, t, digest, batch.msgs[batch.n].data());
                    if (++batch.n == 16) flush();
                }
            }
        }
        if (stop) break;
    }
    if (!stop) flush();
    return count;
}

AVX512_TARGET void GemmGroupsAvx512(const int8_t* q, const int8_t* w, size_t T, size_t din, size_t dout, int32_t* acc)
{
    const size_t ng = din / GROUP;
    // sum(q*w) = sum((q+128)*w) - 128*sum(w); per-(o,g) weight sums once per call.
    std::vector<int32_t> wsum(dout * ng);
    for (size_t o = 0; o < dout; ++o)
        for (size_t g = 0; g < ng; ++g) {
            int32_t s = 0;
            for (size_t k = g * GROUP; k < (g + 1) * GROUP; ++k) s += w[o * din + k];
            wsum[o * ng + g] = s;
        }
    std::vector<uint8_t> qu(din);
    for (size_t t = 0; t < T; ++t) {
        for (size_t k = 0; k < din; ++k) qu[k] = static_cast<uint8_t>(q[t * din + k]) ^ 0x80;
        for (size_t o = 0; o < dout; ++o) {
            const int8_t* wr = w + o * din;
            for (size_t g = 0; g < ng; ++g) {
                __m512i a = _mm512_setzero_si512();
                for (size_t k = g * GROUP; k < (g + 1) * GROUP; k += 64) {
                    a = _mm512_dpbusd_epi32(a, _mm512_loadu_si512(qu.data() + k), _mm512_loadu_si512(wr + k));
                }
                acc[(t * dout + o) * ng + g] = _mm512_reduce_add_epi32(a) - 128 * wsum[o * ng + g];
            }
        }
    }
}
#endif // APERTURE_X86_KERNELS

} // namespace

bool BackendAvailable(Backend backend)
{
    switch (backend) {
    case Backend::SCALAR:
        return true;
    case Backend::AVX512_VNNI:
#ifdef APERTURE_X86_KERNELS
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
               __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512vnni");
#else
        return false;
#endif
    }
    return false;
}

Backend BestBackend()
{
    static const Backend best = BackendAvailable(Backend::AVX512_VNNI) ? Backend::AVX512_VNNI : Backend::SCALAR;
    return best;
}

const char* BackendName(Backend backend)
{
    return backend == Backend::AVX512_VNNI ? "avx512-vnni" : "scalar";
}

uint64_t EnumerateTickets(Backend backend, const unsigned char sigma[32], unsigned int r, const std::vector<Op>& ops,
                          const std::vector<OpInput>& inputs, const TicketVisitor& visit)
{
    if (r == 0 || r > MAX_RANK) return 0;
#ifdef APERTURE_X86_KERNELS
    if (backend == Backend::AVX512_VNNI && BackendAvailable(backend)) return EnumerateAvx512(sigma, r, ops, inputs, visit);
#endif
    return EnumerateScalar(sigma, r, ops, inputs, visit);
}

bool SearchNonce(Backend backend, const unsigned char sigma[32], unsigned int r, const std::vector<Op>& ops,
                 const std::vector<OpInput>& inputs, const unsigned char target[32], SearchHit& hit, uint64_t& tickets)
{
    bool found = false;
    tickets = EnumerateTickets(backend, sigma, r, ops, inputs, [&](const Ticket& t, const unsigned char pow[32]) {
        for (int k = 31; k >= 0; --k) {
            if (pow[k] != target[k]) {
                if (pow[k] > target[k]) return true;
                break;
            }
        }
        hit.ticket = t;
        memcpy(hit.pow, pow, 32);
        found = true;
        return false;
    });
    if (!found) return false;
    for (const OpInput& in : inputs) {
        if (in.op != hit.ticket.op) continue;
        const unsigned int din = ops.at(in.op).d_in;
        hit.panel.assign(static_cast<size_t>(r) * r, 0);
        for (unsigned int x = 0; x < r; ++x) {
            const unsigned int row = hit.ticket.i * r + x;
            if (row >= in.rows) continue;
            memcpy(hit.panel.data() + x * r, in.a + static_cast<size_t>(row) * din + hit.ticket.s * r, r);
        }
        break;
    }
    return true;
}

static std::atomic<unsigned int> g_gemm_threads{1};

void SetGemmThreads(unsigned int threads) { g_gemm_threads = std::max(1u, std::min(threads, 64u)); }
unsigned int GetGemmThreads() { return g_gemm_threads; }

static void GemmGroupsOne(Backend backend, const int8_t* q, const int8_t* w, size_t T, size_t d_in, size_t d_out, int32_t* acc)
{
#ifdef APERTURE_X86_KERNELS
    if (backend == Backend::AVX512_VNNI && BackendAvailable(backend)) return GemmGroupsAvx512(q, w, T, d_in, d_out, acc);
#endif
    GemmGroupsScalar(q, w, T, d_in, d_out, acc);
}

void GemmGroups(Backend backend, const int8_t* q, const int8_t* w, size_t T, size_t d_in, size_t d_out, int32_t* acc)
{
    // Rows are independent, so splitting them across threads is bit-identical.
    // Each thread takes at least GEMM_MIN_ROWS rows to amortise thread start-up.
    static constexpr size_t GEMM_MIN_ROWS = 16;
    const size_t threads = std::min<size_t>(g_gemm_threads, T / GEMM_MIN_ROWS);
    if (threads <= 1) return GemmGroupsOne(backend, q, w, T, d_in, d_out, acc);
    const size_t ng = d_in / GROUP, per = (T + threads - 1) / threads;
    std::vector<std::thread> pool;
    for (size_t t0 = per; t0 < T; t0 += per) {
        const size_t rows = std::min(per, T - t0);
        pool.emplace_back([=] { GemmGroupsOne(backend, q + t0 * d_in, w, rows, d_in, d_out, acc + t0 * d_out * ng); });
    }
    GemmGroupsOne(backend, q, w, std::min(per, T), d_in, d_out, acc);
    for (std::thread& th : pool) th.join();
}

void Blake3OneBlock16(Backend backend, const unsigned char* const msgs[16], const size_t lens[16], unsigned char out[16][32])
{
#ifdef APERTURE_X86_KERNELS
    if (backend == Backend::AVX512_VNNI && BackendAvailable(backend)) {
        unsigned char padded[16][64];
        const unsigned char* ptrs[16];
        uint32_t l32[16];
        for (int k = 0; k < 16; ++k) {
            memset(padded[k], 0, 64);
            memcpy(padded[k], msgs[k], lens[k]);
            ptrs[k] = padded[k];
            l32[k] = static_cast<uint32_t>(lens[k]);
        }
        return Hash16(ptrs, l32, out);
    }
#endif
    for (int k = 0; k < 16; ++k) HashOneScalar(msgs[k], lens[k], out[k]);
}

} // namespace matmulpow_v2
