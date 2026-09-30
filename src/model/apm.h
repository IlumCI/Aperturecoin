// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MODEL_APM_H
#define BITCOIN_MODEL_APM_H

#include <uint256.h>

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

/** The .apm protocol-model container (doc/protocol-model.md). */
namespace apm {

struct Tensor {
    std::string dtype;           //!< "int8", "int32" or "int64"
    std::vector<uint64_t> shape;
    const unsigned char* data{nullptr};
    uint64_t nbytes{0};

    const int8_t* i8() const { return reinterpret_cast<const int8_t*>(data); }
    int64_t i64(uint64_t k) const;  //!< little-endian int64 element k
};

struct Config {
    std::string profile, arch, pooling, tokenizer;
    uint32_t hidden_size{0}, intermediate_size{0}, num_hidden_layers{0}, num_attention_heads{0},
        num_key_value_heads{0}, head_dim{0}, vocab_size{0}, max_positions{0}, eos_token_id{0};
    int qmax{0};
};

class Model {
public:
    /** Parse an .apm image; returns false with an error message on failure. */
    bool Load(std::vector<unsigned char> bytes, std::string& error);
    bool LoadFile(const std::string& path, std::string& error);

    const Config& GetConfig() const { return m_config; }
    const Tensor* Get(const std::string& name) const;
    /** BLAKE3 of the file, in the byte order of the hex string (RPC display). */
    const std::string& WeightsRootHex() const { return m_weights_root_hex; }
    const std::string& ModelIdHex() const { return m_model_id_hex; }

private:
    std::vector<unsigned char> m_bytes;
    Config m_config;
    std::map<std::string, Tensor> m_tensors;
    std::string m_weights_root_hex, m_model_id_hex;
};

std::string Blake3Hex(const unsigned char* data, size_t len);

} // namespace apm

#endif // BITCOIN_MODEL_APM_H
