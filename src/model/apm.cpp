// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <model/apm.h>

#include <crypto/blake3/blake3.h>
#include <crypto/common.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <fstream>
#include <iterator>

namespace apm {
namespace {

const unsigned char MAGIC[8] = {'A', 'P', 'M', 'O', 'D', 'E', 'L', '1'};
constexpr uint64_t ALIGN = 64;

uint64_t Pad(uint64_t n) { return (ALIGN - n % ALIGN) % ALIGN; }

uint64_t DtypeSize(const std::string& dt)
{
    if (dt == "int8") return 1;
    if (dt == "int32") return 4;
    if (dt == "int64") return 8;
    return 0;
}

} // namespace

std::string Blake3Hex(const unsigned char* data, size_t len)
{
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data, len);
    unsigned char out[32];
    blake3_hasher_finalize(&h, out, 32);
    return HexStr(Span<const unsigned char>(out, 32));
}

int64_t Tensor::i64(uint64_t k) const { return static_cast<int64_t>(ReadLE64(data + 8 * k)); }

bool Model::LoadFile(const std::string& path, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return Load(std::move(bytes), error);
}

bool Model::Load(std::vector<unsigned char> bytes, std::string& error)
{
    m_bytes = std::move(bytes);
    m_tensors.clear();
    if (m_bytes.size() < 12 || memcmp(m_bytes.data(), MAGIC, 8) != 0) {
        error = "not an .apm file";
        return false;
    }
    const uint32_t hlen = ReadLE32(m_bytes.data() + 8);
    if (12 + static_cast<uint64_t>(hlen) > m_bytes.size()) {
        error = "truncated header";
        return false;
    }
    UniValue header;
    if (!header.read(std::string(m_bytes.begin() + 12, m_bytes.begin() + 12 + hlen)) || !header.isObject()) {
        error = "bad header json";
        return false;
    }
    const uint64_t base = 12 + hlen + Pad(12 + hlen);
    const UniValue& cfg = header["config"];
    const UniValue& tensors = header["tensors"];
    if (!cfg.isObject() || !tensors.isArray()) {
        error = "bad header layout";
        return false;
    }
    auto num = [&](const char* k) -> uint32_t { return cfg[k].isNum() ? static_cast<uint32_t>(cfg[k].get_int64()) : 0; };
    auto str = [&](const char* k) -> std::string { return cfg[k].isStr() ? cfg[k].get_str() : std::string(); };
    m_config.profile = str("profile");
    m_config.arch = str("arch");
    m_config.pooling = str("pooling");
    m_config.tokenizer = str("tokenizer");
    m_config.hidden_size = num("hidden_size");
    m_config.intermediate_size = num("intermediate_size");
    m_config.num_hidden_layers = num("num_hidden_layers");
    m_config.num_attention_heads = num("num_attention_heads");
    m_config.num_key_value_heads = num("num_key_value_heads");
    m_config.head_dim = num("head_dim");
    m_config.vocab_size = num("vocab_size");
    m_config.max_positions = num("max_positions");
    m_config.eos_token_id = num("eos_token_id");
    m_config.qmax = static_cast<int>(num("qmax"));
    for (size_t k = 0; k < tensors.size(); ++k) {
        const UniValue& e = tensors[k];
        if (!e.isArray() || e.size() != 5 || !e[0].isStr() || !e[1].isStr() || !e[2].isArray()) {
            error = "bad tensor entry";
            return false;
        }
        Tensor t;
        t.dtype = e[1].get_str();
        uint64_t count = 1;
        for (size_t d = 0; d < e[2].size(); ++d) {
            t.shape.push_back(static_cast<uint64_t>(e[2][d].get_int64()));
            count *= t.shape.back();
        }
        const uint64_t off = static_cast<uint64_t>(e[3].get_int64());
        t.nbytes = static_cast<uint64_t>(e[4].get_int64());
        if (DtypeSize(t.dtype) == 0 || count * DtypeSize(t.dtype) != t.nbytes || base + off + t.nbytes > m_bytes.size()) {
            error = "bad tensor " + e[0].get_str();
            return false;
        }
        t.data = m_bytes.data() + base + off;
        m_tensors[e[0].get_str()] = t;
    }
    m_weights_root_hex = Blake3Hex(m_bytes.data(), m_bytes.size());
    std::vector<unsigned char> id_pre{'A', 'p', 'e', 'r', 't', 'u', 'r', 'e', 'M', 'o', 'd', 'e', 'l', '/', 'v', '0'};
    const std::vector<unsigned char> root = ParseHex(m_weights_root_hex);
    id_pre.insert(id_pre.end(), root.begin(), root.end());
    m_model_id_hex = Blake3Hex(id_pre.data(), id_pre.size());
    return true;
}

const Tensor* Model::Get(const std::string& name) const
{
    const auto it = m_tensors.find(name);
    return it == m_tensors.end() ? nullptr : &it->second;
}

} // namespace apm
