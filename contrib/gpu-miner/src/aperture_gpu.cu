// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// ApertureMatMul v2 GPU mining kernels (CUDA; HIP through the compat macros).
// Definitions follow src/crypto/matmulpow_v2.cpp exactly; gpu_selftest checks
// every ticket against the CPU kernels.

#include "aperture_gpu.h"

#include <climits>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(__HIP_PLATFORM_AMD__) || defined(__HIPCC__)
#define APERTURE_HIP 1
#include <hip/hip_runtime.h>
#define gpuError_t hipError_t
#define gpuSuccess hipSuccess
#define gpuMalloc hipMalloc
#define gpuFree hipFree
#define gpuMemcpy hipMemcpy
#define gpuMemcpyHostToDevice hipMemcpyHostToDevice
#define gpuMemcpyDeviceToHost hipMemcpyDeviceToHost
#define gpuMemset hipMemset
#define gpuGetDeviceCount hipGetDeviceCount
#define gpuGetDeviceProperties hipGetDeviceProperties
#define gpuDeviceProp hipDeviceProp_t
#define gpuSetDevice hipSetDevice
#define gpuGetErrorString hipGetErrorString
#define gpuDeviceSynchronize hipDeviceSynchronize
#define gpuGetLastError hipGetLastError
#define SHFL_XOR(v, m) __shfl_xor((v), (m), 32)
#define SHFL(v, src) __shfl((v), (src), 32)
__device__ inline int Dp4a(int a, int b, int c) { return __builtin_amdgcn_sdot4(a, b, c, false); }
#else
#include <cuda_runtime.h>
#include <mma.h>
#define gpuError_t cudaError_t
#define gpuSuccess cudaSuccess
#define gpuMalloc cudaMalloc
#define gpuFree cudaFree
#define gpuMemcpy cudaMemcpy
#define gpuMemcpyHostToDevice cudaMemcpyHostToDevice
#define gpuMemcpyDeviceToHost cudaMemcpyDeviceToHost
#define gpuMemset cudaMemset
#define gpuGetDeviceCount cudaGetDeviceCount
#define gpuGetDeviceProperties cudaGetDeviceProperties
#define gpuDeviceProp cudaDeviceProp
#define gpuSetDevice cudaSetDevice
#define gpuGetErrorString cudaGetErrorString
#define gpuDeviceSynchronize cudaDeviceSynchronize
#define gpuGetLastError cudaGetLastError
#define SHFL_XOR(v, m) __shfl_xor_sync(0xffffffffu, (v), (m), 32)
#define SHFL(v, src) __shfl_sync(0xffffffffu, (v), (src), 32)
__device__ inline int Dp4a(int a, int b, int c) { return __dp4a(a, b, c); }
#endif

namespace aperture_gpu {
namespace {

#define GPU_CHECK(call)                                                                              \
    do {                                                                                             \
        const gpuError_t err_ = (call);                                                              \
        if (err_ != gpuSuccess) throw std::runtime_error(std::string(#call) + ": " + gpuGetErrorString(err_)); \
    } while (0)

constexpr uint32_t FLAGS_ROOT_BLOCK = 1 | 2 | 8; // CHUNK_START | CHUNK_END | ROOT
constexpr unsigned int WARPS_PER_BLOCK = 4;

#if defined(__CUDACC__) || defined(__HIPCC__)
#define HD __host__ __device__
#else
#define HD
#endif

HD inline uint32_t Rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
HD inline uint32_t Rotl(uint32_t x, unsigned n) { return n == 0 ? x : (x << n) | (x >> (32 - n)); }

/**
 * One BLAKE3 compression with the IV chaining value; out = 64-byte XOF block.
 * Host and device share this code, so HostCheck() tests it against the
 * reference library on any machine.
 */
HD void Compress(const uint32_t msg[16], uint32_t counter, uint32_t block_len, uint32_t flags, uint32_t out[16])
{
    const uint32_t iv[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                            0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
    uint32_t m[16], v[16];
    for (int k = 0; k < 16; ++k) m[k] = msg[k];
    for (int k = 0; k < 8; ++k) v[k] = iv[k];
    for (int k = 0; k < 4; ++k) v[8 + k] = iv[k];
    v[12] = counter;
    v[13] = 0;
    v[14] = block_len;
    v[15] = flags;
#define G(a, b, c, d, x, y)                     \
    v[a] = v[a] + v[b] + (x);                   \
    v[d] = Rotr(v[d] ^ v[a], 16);               \
    v[c] = v[c] + v[d];                         \
    v[b] = Rotr(v[b] ^ v[c], 12);               \
    v[a] = v[a] + v[b] + (y);                   \
    v[d] = Rotr(v[d] ^ v[a], 8);                \
    v[c] = v[c] + v[d];                         \
    v[b] = Rotr(v[b] ^ v[c], 7);
    for (int r = 0; r < 7; ++r) {
        G(0, 4, 8, 12, m[0], m[1]);
        G(1, 5, 9, 13, m[2], m[3]);
        G(2, 6, 10, 14, m[4], m[5]);
        G(3, 7, 11, 15, m[6], m[7]);
        G(0, 5, 10, 15, m[8], m[9]);
        G(1, 6, 11, 12, m[10], m[11]);
        G(2, 7, 8, 13, m[12], m[13]);
        G(3, 4, 9, 14, m[14], m[15]);
        // BLAKE3 message permutation {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8}.
        const uint32_t p[16] = {m[2], m[6], m[3], m[10], m[7], m[0], m[4], m[13],
                                m[1], m[11], m[12], m[5], m[9], m[14], m[15], m[8]};
        for (int k = 0; k < 16; ++k) m[k] = p[k];
    }
#undef G
    for (int k = 0; k < 8; ++k) {
        out[k] = v[k] ^ v[k + 8];
        out[k + 8] = v[k + 8] ^ iv[k];
    }
}

struct NoiseMsg {
    uint32_t w[16];
    uint32_t len;
};

__global__ void KNoise(NoiseMsg msg, int8_t* out, uint64_t n)
{
    const uint64_t blk = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (blk * 64 >= n) return;
    uint32_t o[16];
    Compress(msg.w, static_cast<uint32_t>(blk), msg.len, FLAGS_ROOT_BLOCK, o);
    for (int b = 0; b < 64; ++b) {
        const uint64_t idx = blk * 64 + b;
        if (idx >= n) break;
        const uint32_t byte = (o[b / 4] >> (8 * (b % 4))) & 0xff;
        out[idx] = static_cast<int8_t>(static_cast<int>(byte % 3) - 1);
    }
}

__global__ void KBuildA(const int8_t* a, unsigned rows, unsigned rows_p, unsigned din, unsigned r,
                        const int8_t* el, const int8_t* er, int8_t* ap)
{
    const uint64_t idx = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (idx >= static_cast<uint64_t>(rows_p) * din) return;
    const unsigned x = idx / din, c = idx % din;
    int v = x < rows ? a[static_cast<uint64_t>(x) * din + c] : 0;
    for (unsigned k = 0; k < r; ++k) v += el[x * r + k] * er[static_cast<uint64_t>(k) * din + c];
    ap[idx] = static_cast<int8_t>(v);
}

__global__ void KBuildW(const int8_t* w, unsigned din, unsigned dout, unsigned r, const int8_t* fl, const int8_t* fr, int8_t* wt)
{
    const uint64_t idx = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (idx >= static_cast<uint64_t>(dout) * din) return;
    const unsigned c = idx / din, k = idx % din;
    int v = w[idx];
    for (unsigned q = 0; q < r; ++q) v += fr[static_cast<uint64_t>(q) * dout + c] * fl[static_cast<uint64_t>(k) * r + q];
    wt[idx] = static_cast<int8_t>(v);
}

struct TileArgs {
    const int8_t* ap;
    const int8_t* wt;
    unsigned din, r, ni, nj, ns;
    uint64_t count;
    uint32_t sigma[8];
    uint32_t op;
    uint32_t target[8];
    uint32_t* pow_out;              //!< enumerate mode: 8 words per ticket
    unsigned long long* found;      //!< search mode: lowest winning ticket index
};

HD inline uint32_t FoldTerm(int32_t p, uint32_t e)
{
    const uint32_t x = static_cast<uint32_t>(p) ^ e;
    const uint32_t y = (x ^ (x >> 15)) * 0x2c1b3c6du;
    return Rotl(y ^ (y >> 12), e % 32);
}

/** Ticket hash words from the seed, ticket indices and the four Fold lanes (56-byte message). */
HD void TicketHashWords(const uint32_t sigma[8], uint32_t op, uint32_t i, uint32_t j, uint32_t s, const uint32_t lanes[4], uint32_t h[16])
{
    uint32_t m[16];
    for (int k = 0; k < 8; ++k) m[k] = sigma[k];
    m[8] = op | (i << 16);
    m[9] = j | (s << 16);
    for (int k = 0; k < 4; ++k) m[10 + k] = lanes[k];
    m[14] = m[15] = 0;
    Compress(m, 0, 56, FLAGS_ROOT_BLOCK, h);
}

/** Lanes of a 32-lane group hold Fold partial sums with e % 4 == lane % 4; finish the ticket. */
__device__ void FinishTicket(const TileArgs& a, uint64_t t, unsigned lane, uint32_t part, unsigned i, unsigned j, unsigned s)
{
    part += SHFL_XOR(part, 4);
    part += SHFL_XOR(part, 8);
    part += SHFL_XOR(part, 16);
    const uint32_t lanes[4] = {SHFL(part, 0), SHFL(part, 1), SHFL(part, 2), SHFL(part, 3)};
    if (lane != 0) return;
    uint32_t h[16];
    TicketHashWords(a.sigma, a.op, i, j, s, lanes, h);
    if (a.pow_out) {
        for (int k = 0; k < 8; ++k) a.pow_out[t * 8 + k] = h[k];
    }
    if (a.found) {
        for (int k = 7; k >= 0; --k) {
            if (h[k] != a.target[k]) {
                if (h[k] > a.target[k]) return;
                break;
            }
        }
        atomicMin(a.found, static_cast<unsigned long long>(t));
    }
}

/** Portable tile path: one 32-lane group per ticket, int8 dot products with dp4a. */
__global__ void KTilesPortable(TileArgs a)
{
    const uint64_t t = (static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x) / 32;
    const unsigned lane = threadIdx.x % 32;
    if (t >= a.count) return; // uniform per 32-lane group
    const unsigned i = t % a.ni, j = (t / a.ni) % a.nj, s = static_cast<unsigned>(t / (static_cast<uint64_t>(a.ni) * a.nj));
    const unsigned r = a.r;
    uint32_t part = 0;
    for (unsigned e = lane; e < r * r; e += 32) {
        const unsigned row = e / r, col = e % r;
        const int* ap = reinterpret_cast<const int*>(a.ap + static_cast<uint64_t>(i * r + row) * a.din + s * r);
        const int* wp = reinterpret_cast<const int*>(a.wt + static_cast<uint64_t>(j * r + col) * a.din + s * r);
        int acc = 0;
        for (unsigned q = 0; q < r / 4; ++q) acc = Dp4a(ap[q], wp[q], acc);
        part += FoldTerm(acc, e);
    }
    FinishTicket(a, t, lane, part, i, j, s);
}

#ifndef APERTURE_HIP
/**
 * Tensor-core tile path for r == 32: one warp per ticket, 2 x 2 WMMA int8
 * m16n16k16 output fragments over two K halves. Operands are staged in
 * shared memory as 16-byte-wide K halves so every fragment pointer is
 * 256-bit aligned.
 */
__global__ void KTilesTensor(TileArgs a)
{
    using namespace nvcuda;
    __shared__ __align__(32) int8_t s_a[WARPS_PER_BLOCK][2][32 * 16];
    __shared__ __align__(32) int8_t s_b[WARPS_PER_BLOCK][2][32 * 16];
    __shared__ __align__(32) int32_t s_p[WARPS_PER_BLOCK][32 * 32];
    const unsigned warp = threadIdx.x / 32, lane = threadIdx.x % 32;
    const uint64_t t = static_cast<uint64_t>(blockIdx.x) * WARPS_PER_BLOCK + warp;
    if (t >= a.count) return;
    const unsigned i = t % a.ni, j = (t / a.ni) % a.nj, s = static_cast<unsigned>(t / (static_cast<uint64_t>(a.ni) * a.nj));
    const int4* arow = reinterpret_cast<const int4*>(a.ap + static_cast<uint64_t>(i * 32 + lane) * a.din + s * 32);
    const int4* wrow = reinterpret_cast<const int4*>(a.wt + static_cast<uint64_t>(j * 32 + lane) * a.din + s * 32);
    // A: row-major rows of 16 K-bytes per half. B: column c = lane (col-major, 16 K-bytes per column).
    reinterpret_cast<int4*>(s_a[warp][0])[lane] = arow[0];
    reinterpret_cast<int4*>(s_a[warp][1])[lane] = arow[1];
    reinterpret_cast<int4*>(s_b[warp][0])[lane] = wrow[0];
    reinterpret_cast<int4*>(s_b[warp][1])[lane] = wrow[1];
    __syncwarp();
    for (int mi = 0; mi < 2; ++mi) {
        for (int ni = 0; ni < 2; ++ni) {
            wmma::fragment<wmma::accumulator, 16, 16, 16, int> acc;
            wmma::fill_fragment(acc, 0);
            for (int h = 0; h < 2; ++h) {
                wmma::fragment<wmma::matrix_a, 16, 16, 16, signed char, wmma::row_major> fa;
                wmma::fragment<wmma::matrix_b, 16, 16, 16, signed char, wmma::col_major> fb;
                wmma::load_matrix_sync(fa, s_a[warp][h] + mi * 16 * 16, 16);
                wmma::load_matrix_sync(fb, s_b[warp][h] + ni * 16 * 16, 16);
                wmma::mma_sync(acc, fa, fb, acc);
            }
            wmma::store_matrix_sync(s_p[warp] + mi * 16 * 32 + ni * 16, acc, 32, wmma::mem_row_major);
        }
    }
    __syncwarp();
    uint32_t part = 0;
    for (unsigned e = lane; e < 32 * 32; e += 32) part += FoldTerm(s_p[warp][e], e);
    FinishTicket(a, t, lane, part, i, j, s);
}
#endif

void LoadWords(const unsigned char* bytes, size_t n, uint32_t* words)
{
    for (size_t k = 0; k < n / 4; ++k) {
        words[k] = uint32_t{bytes[4 * k]} | (uint32_t{bytes[4 * k + 1]} << 8) | (uint32_t{bytes[4 * k + 2]} << 16) |
                   (uint32_t{bytes[4 * k + 3]} << 24);
    }
}

NoiseMsg MakeNoiseMsg(const unsigned char sigma[32], uint16_t op, matmulpow_v2::Factor f)
{
    static const char TAG[] = "ApertureMatMul/v2/noise";
    unsigned char msg[64] = {0};
    const size_t tag = sizeof(TAG) - 1;
    memcpy(msg, TAG, tag);
    memcpy(msg + tag, sigma, 32);
    msg[tag + 32] = op & 0xff;
    msg[tag + 33] = op >> 8;
    msg[tag + 34] = static_cast<unsigned char>(f);
    NoiseMsg m;
    LoadWords(msg, 64, m.w);
    m.len = static_cast<uint32_t>(tag + 35);
    return m;
}

template <typename T>
struct DeviceBuffer {
    T* ptr{nullptr};
    size_t n{0};
    void Reserve(size_t count)
    {
        if (count <= n) return;
        if (ptr) gpuFree(ptr);
        ptr = nullptr;
        GPU_CHECK(gpuMalloc(&ptr, count * sizeof(T)));
        n = count;
    }
    ~DeviceBuffer()
    {
        if (ptr) gpuFree(ptr);
    }
};

unsigned Blocks(uint64_t threads, unsigned per_block) { return static_cast<unsigned>((threads + per_block - 1) / per_block); }

} // namespace

bool HostCheck(std::string& report)
{
    // Runs the device math (Compress, FoldTerm with the 32-lane grouping and
    // xor-shuffle reduction, the ticket message) on the host and compares it
    // with the consensus reference.
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
    int bad = 0;
    for (int trial = 0; trial < 64; ++trial) {
        unsigned char sigma[32];
        for (auto& b : sigma) b = rnd() & 0xff;
        const unsigned r = trial % 2 ? 32 : 8;
        std::vector<int32_t> p(r * r);
        for (auto& v : p) v = static_cast<int32_t>(rnd());
        matmulpow_v2::Ticket t;
        t.op = rnd() & 0xffff;
        t.i = rnd() & 0xffff;
        t.j = rnd() & 0xffff;
        t.s = rnd() & 0xffff;
        unsigned char digest[16], want[32];
        matmulpow_v2::Fold(p.data(), p.size(), digest);
        matmulpow_v2::TicketHash(sigma, t, digest, want);
        uint32_t lane_part[32] = {0};
        for (unsigned e = 0; e < r * r; ++e) lane_part[e % 32] += FoldTerm(p[e], e);
        for (int off : {4, 8, 16}) {
            uint32_t next[32];
            for (int l = 0; l < 32; ++l) next[l] = lane_part[l] + lane_part[l ^ off];
            memcpy(lane_part, next, sizeof(next));
        }
        uint32_t sw[8], h[16];
        LoadWords(sigma, 32, sw);
        TicketHashWords(sw, t.op, t.i, t.j, t.s, lane_part, h);
        unsigned char got[32];
        for (int w = 0; w < 8; ++w)
            for (int b = 0; b < 4; ++b) got[4 * w + b] = (h[w] >> (8 * b)) & 0xff;
        bad += memcmp(got, want, 32) != 0;
        // Noise stream block: Compress-based XOF against the reference Noise().
        const auto f = static_cast<matmulpow_v2::Factor>(trial % 4);
        const uint64_t offset = 64 * (rnd() % 1000);
        std::vector<int8_t> ref(64);
        matmulpow_v2::Noise(sigma, t.op, f, offset, 64, ref.data());
        const NoiseMsg msg = MakeNoiseMsg(sigma, t.op, f);
        uint32_t o[16];
        Compress(msg.w, static_cast<uint32_t>(offset / 64), msg.len, FLAGS_ROOT_BLOCK, o);
        for (int b = 0; b < 64; ++b) {
            const uint32_t byte = (o[b / 4] >> (8 * (b % 4))) & 0xff;
            bad += ref[b] != static_cast<int8_t>(static_cast<int>(byte % 3) - 1);
        }
    }
    report = bad ? "device math MISMATCH on the host" : "device math (BLAKE3, Fold lanes, ticket hash, noise) matches the reference on the host";
    return bad == 0;
}

std::vector<DeviceInfo> Devices(std::string* error)
{
    std::vector<DeviceInfo> out;
    int count = 0;
    const gpuError_t err = gpuGetDeviceCount(&count);
    if (err != gpuSuccess) {
        if (error) *error = gpuGetErrorString(err);
        return out;
    }
    for (int d = 0; d < count; ++d) {
        gpuDeviceProp prop;
        if (gpuGetDeviceProperties(&prop, d) != gpuSuccess) continue;
        out.push_back(DeviceInfo{d, prop.name, prop.major, prop.minor});
    }
    return out;
}

struct Miner::Impl {
    unsigned int r;
    std::vector<matmulpow_v2::Op> ops;
    std::vector<DeviceBuffer<int8_t>> weights;
    std::vector<matmulpow_v2::OpInput> inputs;
    std::vector<DeviceBuffer<int8_t>> acts;
    DeviceBuffer<int8_t> el, er, fl, fr, ap, wt;
    DeviceBuffer<uint32_t> pow_out;
    DeviceBuffer<unsigned long long> found;

    /** Run noise, operand construction and tiles for input k. */
    void RunOp(size_t k, const unsigned char sigma[32], TilePath path, const uint32_t target[8], bool enumerate, uint64_t& count)
    {
        const matmulpow_v2::OpInput& in = inputs[k];
        const matmulpow_v2::Op& od = ops.at(in.op);
        const unsigned din = od.d_in, dout = od.d_out, rows_p = (in.rows + r - 1) / r * r;
        const uint64_t n_el = uint64_t{rows_p} * r, n_er = uint64_t{r} * din, n_fl = uint64_t{din} * r, n_fr = uint64_t{r} * dout;
        el.Reserve(n_el);
        er.Reserve(n_er);
        fl.Reserve(n_fl);
        fr.Reserve(n_fr);
        ap.Reserve(uint64_t{rows_p} * din);
        wt.Reserve(uint64_t{dout} * din);
        const struct { matmulpow_v2::Factor f; int8_t* p; uint64_t n; } streams[4] = {
            {matmulpow_v2::Factor::EL, el.ptr, n_el}, {matmulpow_v2::Factor::ER, er.ptr, n_er},
            {matmulpow_v2::Factor::FL, fl.ptr, n_fl}, {matmulpow_v2::Factor::FR, fr.ptr, n_fr}};
        for (const auto& st : streams) {
            KNoise<<<Blocks((st.n + 63) / 64, 256), 256>>>(MakeNoiseMsg(sigma, in.op, st.f), st.p, st.n);
        }
        KBuildA<<<Blocks(uint64_t{rows_p} * din, 256), 256>>>(acts[k].ptr, in.rows, rows_p, din, r, el.ptr, er.ptr, ap.ptr);
        KBuildW<<<Blocks(uint64_t{dout} * din, 256), 256>>>(weights[in.op].ptr, din, dout, r, fl.ptr, fr.ptr, wt.ptr);

        TileArgs args{};
        args.ap = ap.ptr;
        args.wt = wt.ptr;
        args.din = din;
        args.r = r;
        args.ni = rows_p / r;
        args.nj = dout / r;
        args.ns = din / r;
        args.count = uint64_t{args.ni} * args.nj * args.ns;
        LoadWords(sigma, 32, args.sigma);
        args.op = in.op;
        memcpy(args.target, target, sizeof(args.target));
        if (enumerate) {
            pow_out.Reserve(args.count * 8);
            args.pow_out = pow_out.ptr;
        } else {
            const unsigned long long none = ULLONG_MAX;
            GPU_CHECK(gpuMemcpy(found.ptr, &none, sizeof(none), gpuMemcpyHostToDevice));
            args.found = found.ptr;
        }
#ifndef APERTURE_HIP
        if (path == TilePath::TENSOR_CORE && r == 32) {
            KTilesTensor<<<Blocks(args.count, WARPS_PER_BLOCK), WARPS_PER_BLOCK * 32>>>(args);
        } else
#endif
        {
            KTilesPortable<<<Blocks(args.count * 32, 128), 128>>>(args);
        }
        GPU_CHECK(gpuGetLastError());
        GPU_CHECK(gpuDeviceSynchronize());
        count = args.count;
    }
};

Miner::Miner(int device, unsigned int rank, const std::vector<matmulpow_v2::Op>& ops) : m_impl(new Impl)
{
    if (rank == 0 || rank > matmulpow_v2::MAX_RANK || rank % 4 != 0) throw std::runtime_error("unsupported rank");
    GPU_CHECK(gpuSetDevice(device));
    m_impl->r = rank;
    m_impl->ops = ops;
    m_impl->weights.resize(ops.size());
    for (size_t k = 0; k < ops.size(); ++k) {
        const size_t n = size_t{ops[k].d_out} * ops[k].d_in;
        m_impl->weights[k].Reserve(n);
        GPU_CHECK(gpuMemcpy(m_impl->weights[k].ptr, ops[k].w, n, gpuMemcpyHostToDevice));
    }
    m_impl->found.Reserve(1);
}

Miner::~Miner() = default;

void Miner::SetInputs(const std::vector<matmulpow_v2::OpInput>& inputs)
{
    m_impl->inputs = inputs;
    m_impl->acts.clear();
    m_impl->acts.resize(inputs.size());
    for (size_t k = 0; k < inputs.size(); ++k) {
        const size_t n = size_t{inputs[k].rows} * m_impl->ops.at(inputs[k].op).d_in;
        m_impl->acts[k].Reserve(n == 0 ? 1 : n);
        if (n) GPU_CHECK(gpuMemcpy(m_impl->acts[k].ptr, inputs[k].a, n, gpuMemcpyHostToDevice));
    }
}

bool Miner::Search(const unsigned char sigma[32], const unsigned char target[32], TilePath path,
                   matmulpow_v2::SearchHit& hit, uint64_t& tickets)
{
    uint32_t tw[8];
    LoadWords(target, 32, tw);
    tickets = 0;
    for (size_t k = 0; k < m_impl->inputs.size(); ++k) {
        uint64_t count = 0;
        m_impl->RunOp(k, sigma, path, tw, false, count);
        unsigned long long found = 0;
        GPU_CHECK(gpuMemcpy(&found, m_impl->found.ptr, sizeof(found), gpuMemcpyDeviceToHost));
        if (found == ULLONG_MAX) {
            tickets += count;
            continue;
        }
        tickets += found + 1;
        const matmulpow_v2::OpInput& in = m_impl->inputs[k];
        const matmulpow_v2::Op& od = m_impl->ops.at(in.op);
        const unsigned r = m_impl->r, ni = (in.rows + r - 1) / r, nj = od.d_out / r;
        hit.ticket.op = in.op;
        hit.ticket.i = static_cast<uint16_t>(found % ni);
        hit.ticket.j = static_cast<uint16_t>((found / ni) % nj);
        hit.ticket.s = static_cast<uint16_t>(found / (uint64_t{ni} * nj));
        hit.panel.assign(size_t{r} * r, 0);
        for (unsigned x = 0; x < r; ++x) {
            const unsigned row = hit.ticket.i * r + x;
            if (row < in.rows) memcpy(hit.panel.data() + x * r, in.a + size_t{row} * od.d_in + hit.ticket.s * r, r);
        }
        // The hit is re-verified with the consensus reference before it is reported.
        if (!matmulpow_v2::TicketPoW(sigma, r, od, hit.ticket, hit.panel.data(), hit.pow)) {
            throw std::runtime_error("GPU ticket failed reference verification");
        }
        for (int b = 31; b >= 0; --b) {
            if (hit.pow[b] != target[b]) {
                if (hit.pow[b] > target[b]) throw std::runtime_error("GPU reported a ticket above the target");
                break;
            }
        }
        return true;
    }
    return false;
}

std::vector<std::array<unsigned char, 32>> Miner::Enumerate(const unsigned char sigma[32], TilePath path)
{
    std::vector<std::array<unsigned char, 32>> out;
    const uint32_t none[8] = {0};
    for (size_t k = 0; k < m_impl->inputs.size(); ++k) {
        uint64_t count = 0;
        m_impl->RunOp(k, sigma, path, none, true, count);
        std::vector<uint32_t> words(count * 8);
        GPU_CHECK(gpuMemcpy(words.data(), m_impl->pow_out.ptr, words.size() * 4, gpuMemcpyDeviceToHost));
        for (uint64_t t = 0; t < count; ++t) {
            std::array<unsigned char, 32> h;
            for (int w = 0; w < 8; ++w) {
                const uint32_t v = words[t * 8 + w];
                h[4 * w] = v & 0xff;
                h[4 * w + 1] = (v >> 8) & 0xff;
                h[4 * w + 2] = (v >> 16) & 0xff;
                h[4 * w + 3] = v >> 24;
            }
            out.push_back(h);
        }
    }
    return out;
}

} // namespace aperture_gpu
