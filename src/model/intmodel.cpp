// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <model/intmodel.h>

#include <crypto/blake3/blake3.h>
#include <crypto/common.h>
#include <crypto/matmulpow_v2_kernel.h>
#include <tinyformat.h>

#include <algorithm>
#include <cmath>

namespace intmodel {
namespace {

constexpr int64_t LN2_Q16 = 45426;
constexpr int64_t EPS_Q32 = 4295;
constexpr int64_t EXP_A_Q16 = 23495;
constexpr int64_t EXP_B_Q16 = 88670;
constexpr int64_t EXP_C_Q16 = 22544;
const char* const PROJ[7] = {"q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"};

int BitLen(uint64_t x)
{
    int n = 0;
    while (x) {
        ++n;
        x >>= 1;
    }
    return n;
}

using Mat = std::vector<int64_t>; // row-major

void RmsNormRow(int64_t* x, size_t d, const apm::Tensor& g)
{
    int64_t mx = 0;
    for (size_t c = 0; c < d; ++c) mx = std::max(mx, x[c] < 0 ? -x[c] : x[c]);
    const int shift = std::max(BitLen(static_cast<uint64_t>(mx)) - 24, 0);
    int64_t ss = 0;
    for (size_t c = 0; c < d; ++c) {
        const int64_t xs = x[c] >> shift;
        ss += xs * xs;
    }
    const int64_t mean = ss / static_cast<int64_t>(d);
    int64_t rms = ISqrt(mean + (EPS_Q32 >> std::min(2 * shift, 62)));
    rms = std::max<int64_t>(rms, 1) << shift;
    for (size_t c = 0; c < d; ++c) x[c] = RDiv(x[c] * g.i64(c), rms);
}

} // namespace

int64_t FloorDiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

int64_t RDiv(int64_t a, int64_t b) { return FloorDiv(2 * a + b, 2 * b); }

int64_t ISqrt(int64_t n)
{
    int64_t r = static_cast<int64_t>(std::sqrt(static_cast<double>(n)));
    for (int k = 0; k < 3; ++k) {
        if (r * r > n) --r;
        if ((r + 1) * (r + 1) <= n) ++r;
    }
    return r;
}

int64_t IExpNeg(int64_t z)
{
    const int64_t k = FloorDiv(-z, LN2_Q16);
    if (k >= 62) return 0;
    const int64_t p = z + k * LN2_Q16;
    const int64_t t = p + EXP_B_Q16;
    const int64_t poly = RDiv(EXP_A_Q16 * RDiv(t * t, ONE), ONE) + EXP_C_Q16;
    return poly >> k;
}

int64_t Sigmoid(int64_t x)
{
    const int64_t e = IExpNeg(x < 0 ? x : -x);
    const int64_t s = RDiv(ONE * ONE, ONE + e);
    return x >= 0 ? s : ONE - s;
}

int64_t Silu(int64_t x) { return RDiv(x * Sigmoid(x), ONE); }

bool IntModel::Init(std::unique_ptr<apm::Model> model, std::string& error)
{
    m_model = std::move(model);
    const apm::Config& c = m_model->GetConfig();
    if (c.profile != "aperture-int-v0" || c.arch != "qwen3-embedding" || c.qmax != QMAX) {
        error = "unsupported model profile/arch";
        return false;
    }
    if (c.num_attention_heads == 0 || c.num_key_value_heads == 0 || c.num_attention_heads % c.num_key_value_heads != 0 ||
        c.head_dim % 2 != 0 || c.max_positions == 0) {
        error = "bad model config";
        return false;
    }
    m_ops.clear();
    m_scales.clear();
    const uint32_t H = c.hidden_size, FF = c.intermediate_size, QD = c.num_attention_heads * c.head_dim,
                   KD = c.num_key_value_heads * c.head_dim;
    const uint32_t shapes[7][2] = {{QD, H}, {KD, H}, {KD, H}, {H, QD}, {FF, H}, {FF, H}, {H, FF}};
    for (uint32_t l = 0; l < c.num_hidden_layers; ++l) {
        for (int p = 0; p < 7; ++p) {
            const std::string base = strprintf("layers.%u.%s", l, PROJ[p]);
            const apm::Tensor* q = m_model->Get(base + ".q");
            const apm::Tensor* s = m_model->Get(base + ".s");
            if (!q || !s || q->dtype != "int8" || s->dtype != "int64" || q->shape.size() != 2 ||
                q->shape[0] != shapes[p][0] || q->shape[1] != shapes[p][1] || s->shape[0] != shapes[p][0] ||
                shapes[p][1] % GROUP != 0) {
                error = "bad tensor " + base;
                return false;
            }
            m_ops.push_back(matmulpow_v2::Op{shapes[p][1], shapes[p][0], q->i8()});
            m_scales.push_back(s);
        }
    }
    for (const char* name : {"embed.q", "embed.s", "norm", "rope.cos", "rope.sin"}) {
        if (!m_model->Get(name)) {
            error = std::string("missing tensor ") + name;
            return false;
        }
    }
    m_isq = std::llround(static_cast<double>(ONE) / std::sqrt(static_cast<double>(c.head_dim)));
    return true;
}

std::vector<uint32_t> IntModel::TokenizeBytes(const std::string& text) const
{
    std::vector<uint32_t> ids;
    for (unsigned char ch : text) ids.push_back(ch);
    ids.push_back(Config().eos_token_id);
    return ids;
}

bool IntModel::ValidInput(const std::vector<uint32_t>& ids) const
{
    const apm::Config& c = Config();
    if (ids.empty() || ids.size() > c.max_positions) return false;
    for (uint32_t id : ids) {
        if (id >= c.vocab_size) return false;
    }
    return true;
}

std::vector<int64_t> IntModel::Tokens(const std::vector<uint32_t>& ids) const
{
    const apm::Config& c = Config();
    const size_t T = ids.size(), H = c.hidden_size;
    const apm::Tensor& eq = *m_model->Get("embed.q");
    const apm::Tensor& es = *m_model->Get("embed.s");
    Mat x(T * H);
    for (size_t t = 0; t < T; ++t)
        for (size_t k = 0; k < H; ++k)
            x[t * H + k] = RDiv(int64_t{eq.i8()[static_cast<size_t>(ids[t]) * H + k]} * es.i64(ids[t]), int64_t{1} << (WS_SHIFT - FRAC));

    return x;
}

void IntModel::Layer(uint32_t l, std::vector<int64_t>& x, size_t T, std::vector<OpTrace>* trace) const
{
    const apm::Config& c = Config();
    const size_t H = c.hidden_size, NH = c.num_attention_heads, KV = c.num_key_value_heads, D = c.head_dim;
    auto linear = [&](const Mat& in, size_t din, uint16_t op) {
        const matmulpow_v2::Op& od = m_ops[op];
        const apm::Tensor& ws = *m_scales[op];
        const size_t dout = od.d_out, ng = din / GROUP;
        std::vector<int8_t> q(T * din);
        std::vector<int64_t> m(T * ng);
        for (size_t t = 0; t < T; ++t) {
            for (size_t g = 0; g < ng; ++g) {
                int64_t mx = 0;
                for (size_t k = 0; k < GROUP; ++k) {
                    const int64_t v = in[t * din + g * GROUP + k];
                    mx = std::max(mx, v < 0 ? -v : v);
                }
                m[t * ng + g] = mx;
                const int64_t safe = mx == 0 ? 1 : mx;
                for (size_t k = 0; k < GROUP; ++k)
                    q[t * din + g * GROUP + k] = static_cast<int8_t>(RDiv(in[t * din + g * GROUP + k] * QMAX, safe));
            }
        }
        // Exact per-group int8 products (SIMD kernel, bit-identical to the scalar definition).
        std::vector<int32_t> accs(T * dout * ng);
        matmulpow_v2::GemmGroups(matmulpow_v2::BestBackend(), q.data(), od.w, T, din, dout, accs.data());
        Mat out(T * dout, 0);
        for (size_t t = 0; t < T; ++t) {
            for (size_t o = 0; o < dout; ++o) {
                const int64_t s = ws.i64(o);
                int64_t y = 0;
                for (size_t g = 0; g < ng; ++g) {
                    const int64_t tt = RDiv(int64_t{accs[(t * dout + o) * ng + g]} * s, int64_t{1} << WS_SHIFT);
                    y += RDiv(tt * m[t * ng + g], QMAX);
                }
                out[t * dout + o] = y;
            }
        }
        if (trace) trace->push_back(OpTrace{op, static_cast<unsigned int>(T), static_cast<unsigned int>(din), std::move(q)});
        return out;
    };

    const apm::Tensor& rc = *m_model->Get("rope.cos");
    const apm::Tensor& rs = *m_model->Get("rope.sin");
    const size_t half = D / 2;
    auto rope_norm = [&](Mat& v, size_t heads, const apm::Tensor& g) {
        for (size_t t = 0; t < T; ++t) {
            for (size_t h = 0; h < heads; ++h) {
                int64_t* row = &v[(t * heads + h) * D];
                RmsNormRow(row, D, g);
                std::vector<int64_t> r(D);
                for (size_t f = 0; f < half; ++f) {
                    const int64_t cs = rc.i64(t * half + f), sn = rs.i64(t * half + f);
                    r[f] = RDiv(row[f] * cs - row[f + half] * sn, ONE);
                    r[f + half] = RDiv(row[f + half] * cs + row[f] * sn, ONE);
                }
                std::copy(r.begin(), r.end(), row);
            }
        }
    };

    const std::string p = strprintf("layers.%u.", l);
    const uint16_t op0 = static_cast<uint16_t>(l * 7);
    Mat h = x;
    for (size_t t = 0; t < T; ++t) RmsNormRow(&h[t * H], H, *m_model->Get(p + "input_norm"));
    Mat q = linear(h, H, op0 + 0), k = linear(h, H, op0 + 1), v = linear(h, H, op0 + 2);
    rope_norm(q, NH, *m_model->Get(p + "q_norm"));
    rope_norm(k, KV, *m_model->Get(p + "k_norm"));
    Mat att(T * NH * D, 0);
    std::vector<int64_t> sc(T), pr(T);
    for (size_t hh = 0; hh < NH; ++hh) {
        const size_t kv = hh / (NH / KV);
        for (size_t a = 0; a < T; ++a) {
            int64_t mx = INT64_MIN;
            for (size_t b = 0; b <= a; ++b) {
                int64_t dot = 0;
                for (size_t d = 0; d < D; ++d) dot += q[(a * NH + hh) * D + d] * k[(b * KV + kv) * D + d];
                sc[b] = RDiv(RDiv(dot, ONE) * m_isq, ONE);
                mx = std::max(mx, sc[b]);
            }
            int64_t tot = 0;
            for (size_t b = 0; b <= a; ++b) {
                pr[b] = IExpNeg(sc[b] - mx);
                tot += pr[b];
            }
            for (size_t b = 0; b <= a; ++b) pr[b] = RDiv(pr[b] * ONE, tot);
            for (size_t d = 0; d < D; ++d) {
                int64_t acc = 0;
                for (size_t b = 0; b <= a; ++b) acc += pr[b] * v[(b * KV + kv) * D + d];
                att[(a * NH + hh) * D + d] = RDiv(acc, ONE);
            }
        }
    }
    const Mat o = linear(att, NH * D, op0 + 3);
    for (size_t z = 0; z < x.size(); ++z) x[z] += o[z];
    h = x;
    for (size_t t = 0; t < T; ++t) RmsNormRow(&h[t * H], H, *m_model->Get(p + "post_norm"));
    const size_t FF = c.intermediate_size;
    Mat g = linear(h, H, op0 + 4);
    const Mat u = linear(h, H, op0 + 5);
    for (size_t z = 0; z < g.size(); ++z) g[z] = RDiv(Silu(g[z]) * u[z], ONE);
    const Mat dn = linear(g, FF, op0 + 6);
    for (size_t z = 0; z < x.size(); ++z) x[z] += dn[z];
}

std::vector<int8_t> IntModel::Final(std::vector<int64_t> x, size_t T) const
{
    const size_t H = Config().hidden_size;
    int64_t* last = &x[(T - 1) * H];
    RmsNormRow(last, H, *m_model->Get("norm"));
    int64_t mx = 0;
    for (size_t k = 0; k < H; ++k) mx = std::max(mx, last[k] < 0 ? -last[k] : last[k]);
    std::vector<int8_t> out(H);
    for (size_t k = 0; k < H; ++k) out[k] = static_cast<int8_t>(RDiv(last[k] * 127, mx == 0 ? 1 : mx));
    return out;
}


IntModel::StateHash IntModel::HashState(uint32_t index, const std::vector<int64_t>& x)
{
    static const char TAG[] = "ApertureState/v0";
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, TAG, sizeof(TAG) - 1);
    unsigned char b[8];
    WriteLE32(b, index);
    blake3_hasher_update(&h, b, 4);
    for (int64_t v : x) {
        WriteLE64(b, static_cast<uint64_t>(v));
        blake3_hasher_update(&h, b, 8);
    }
    StateHash out;
    blake3_hasher_finalize(&h, out.data(), out.size());
    return out;
}

std::vector<int8_t> IntModel::Embed(const std::vector<uint32_t>& ids, std::vector<OpTrace>* trace, std::vector<StateHash>* states) const
{
    if (!ValidInput(ids)) return {};
    const size_t T = ids.size();
    std::vector<int64_t> x = Tokens(ids);
    if (states) states->assign(1, HashState(0, x));
    for (uint32_t l = 0; l < Config().num_hidden_layers; ++l) {
        Layer(l, x, T, trace);
        if (states) states->push_back(HashState(l + 1, x));
    }
    return Final(std::move(x), T);
}

// ------------------------------------------------------------------ tiny --

namespace {

class Xof {
public:
    explicit Xof(const std::vector<unsigned char>& seed)
    {
        blake3_hasher_init(&m_h);
        blake3_hasher_update(&m_h, seed.data(), seed.size());
    }
    std::vector<unsigned char> Read(size_t n)
    {
        std::vector<unsigned char> out(n);
        blake3_hasher_finalize_seek(&m_h, m_pos, out.data(), n);
        m_pos += n;
        return out;
    }

private:
    blake3_hasher m_h;
    uint64_t m_pos{0};
};

struct RawTensor {
    std::string name, dtype;
    std::vector<uint64_t> shape;
    std::vector<unsigned char> bytes;
};

void PushI64(RawTensor& t, int64_t v)
{
    unsigned char b[8];
    WriteLE64(b, static_cast<uint64_t>(v));
    t.bytes.insert(t.bytes.end(), b, b + 8);
}

} // namespace

std::vector<unsigned char> BuildTinyModel(uint32_t seed)
{
    const uint32_t H = 256, L = 2, NH = 2, KV = 1, D = 128, FF = 768, V = 257, MAXPOS = 64;
    std::vector<unsigned char> xs{'A', 'p', 'e', 'r', 't', 'u', 'r', 'e', 'T', 'i', 'n', 'y', 'M', 'o', 'd', 'e', 'l', '/', 'v', '0'};
    unsigned char sb[4];
    WriteLE32(sb, seed);
    xs.insert(xs.end(), sb, sb + 4);
    Xof xof(xs);
    std::vector<RawTensor> tensors;

    auto weight = [&](const std::string& name, uint32_t dout, uint32_t din, int qmax) {
        RawTensor q{name + ".q", "int8", {dout, din}, xof.Read(static_cast<size_t>(dout) * din)};
        for (auto& b : q.bytes) b = static_cast<unsigned char>(static_cast<int8_t>(static_cast<int>(b % (2 * qmax + 1)) - qmax));
        const std::vector<unsigned char> u = xof.Read(4 * static_cast<size_t>(dout));
        RawTensor s{name + ".s", "int64", {dout}, {}};
        for (uint32_t k = 0; k < dout; ++k) PushI64(s, (int64_t{1} << 20) + ReadLE32(u.data() + 4 * k) % (uint32_t{3} << 20));
        tensors.push_back(std::move(q));
        tensors.push_back(std::move(s));
    };
    auto norm = [&](const std::string& name, uint32_t d) {
        const std::vector<unsigned char> u = xof.Read(4 * static_cast<size_t>(d));
        RawTensor t{name, "int64", {d}, {}};
        for (uint32_t k = 0; k < d; ++k) PushI64(t, ONE / 2 + ReadLE32(u.data() + 4 * k) % (3 * ONE / 2));
        tensors.push_back(std::move(t));
    };

    weight("embed", V, H, 127);
    const uint32_t shapes[7][2] = {{NH * D, H}, {KV * D, H}, {KV * D, H}, {H, NH * D}, {FF, H}, {FF, H}, {H, FF}};
    for (uint32_t i = 0; i < L; ++i) {
        const std::string p = strprintf("layers.%u.", i);
        norm(p + "input_norm", H);
        norm(p + "post_norm", H);
        norm(p + "q_norm", D);
        norm(p + "k_norm", D);
        for (int pr = 0; pr < 7; ++pr) weight(p + PROJ[pr], shapes[pr][0], shapes[pr][1], QMAX);
    }
    norm("norm", H);
    RawTensor cs{"rope.cos", "int64", {MAXPOS, D / 2}, {}}, sn{"rope.sin", "int64", {MAXPOS, D / 2}, {}};
    for (uint32_t pos = 0; pos < MAXPOS; ++pos) {
        for (uint32_t f = 0; f < D / 2; ++f) {
            const double inv = 1.0 / std::pow(1e6, static_cast<double>(2 * f) / D);
            const double ang = static_cast<double>(pos) * inv;
            PushI64(cs, static_cast<int64_t>(std::nearbyint(std::cos(ang) * ONE)));
            PushI64(sn, static_cast<int64_t>(std::nearbyint(std::sin(ang) * ONE)));
        }
    }
    tensors.push_back(std::move(cs));
    tensors.push_back(std::move(sn));

    // Canonical JSON header, identical to json.dumps(sort_keys=True, separators=(",", ":")).
    const unsigned char bytes_tok[] = {'b', 'y', 't', 'e', 's'};
    std::string header = "{\"config\":{";
    header += "\"arch\":\"qwen3-embedding\",\"eos_token_id\":256,";
    header += strprintf("\"head_dim\":%u,\"hidden_size\":%u,\"intermediate_size\":%u,\"max_positions\":%u,", D, H, FF, MAXPOS);
    header += strprintf("\"num_attention_heads\":%u,\"num_hidden_layers\":%u,\"num_key_value_heads\":%u,", NH, L, KV);
    header += "\"pooling\":\"last_token\",\"profile\":\"aperture-int-v0\",";
    header += strprintf("\"qmax\":%d,\"source\":\"tiny_model(seed=%u)\",\"tokenizer\":\"bytes\",", QMAX, seed);
    header += strprintf("\"tokenizer_blake3\":\"%s\",\"vocab_size\":%u},\"tensors\":[", apm::Blake3Hex(bytes_tok, sizeof(bytes_tok)), V);
    uint64_t off = 0;
    for (size_t k = 0; k < tensors.size(); ++k) {
        const RawTensor& t = tensors[k];
        std::string shape;
        for (size_t d = 0; d < t.shape.size(); ++d) shape += (d ? "," : "") + strprintf("%u", t.shape[d]);
        header += strprintf("%s[\"%s\",\"%s\",[%s],%u,%u]", k ? "," : "", t.name, t.dtype, shape, off, t.bytes.size());
        off += t.bytes.size() + (64 - t.bytes.size() % 64) % 64;
    }
    header += "]}";

    std::vector<unsigned char> out{'A', 'P', 'M', 'O', 'D', 'E', 'L', '1'};
    unsigned char hl[4];
    WriteLE32(hl, static_cast<uint32_t>(header.size()));
    out.insert(out.end(), hl, hl + 4);
    out.insert(out.end(), header.begin(), header.end());
    out.resize(out.size() + (64 - out.size() % 64) % 64, 0);
    for (const RawTensor& t : tensors) {
        out.insert(out.end(), t.bytes.begin(), t.bytes.end());
        out.resize(out.size() + (64 - t.bytes.size() % 64) % 64, 0);
    }
    return out;
}

} // namespace intmodel
