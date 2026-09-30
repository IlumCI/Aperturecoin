// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MODEL_INTMODEL_H
#define BITCOIN_MODEL_INTMODEL_H

#include <crypto/matmulpow_v2.h>
#include <model/apm.h>

#include <stdint.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
 * Integer inference profile "aperture-int-v0" (doc/protocol-model.md). This is
 * a line-by-line port of contrib/aperture-model/aperture_model/intops.py and
 * IntModel; the two must agree bit for bit (golden vectors in
 * src/test/protocolmodel_tests.cpp and contrib/aperture-model/test).
 */
namespace intmodel {

static constexpr int FRAC = 16;
static constexpr int64_t ONE = int64_t{1} << FRAC;
static constexpr int QMAX = 95;
static constexpr unsigned int GROUP = 256;
static constexpr int WS_SHIFT = 30;

int64_t FloorDiv(int64_t a, int64_t b);
int64_t RDiv(int64_t a, int64_t b);        //!< round half up, b > 0
int64_t ISqrt(int64_t n);                  //!< floor sqrt, 0 <= n < 2^62
int64_t IExpNeg(int64_t z);                //!< exp(z) Q16 for z <= 0 Q16
int64_t Sigmoid(int64_t x);
int64_t Silu(int64_t x);

/** Quantized input of one weight matmul (the PoW activation operand). */
struct OpTrace {
    uint16_t op;
    unsigned int rows;
    unsigned int d_in;
    std::vector<int8_t> q;   //!< rows x d_in, |q| <= QMAX
};

class IntModel {
public:
    /** Takes ownership of a loaded .apm model (arch "qwen3-embedding"). */
    bool Init(std::unique_ptr<apm::Model> model, std::string& error);

    const apm::Config& Config() const { return m_model->GetConfig(); }
    const apm::Model& Apm() const { return *m_model; }

    /** Weight matmuls in forward order: op = layer * 7 + {q,k,v,o,gate,up,down}. */
    const std::vector<matmulpow_v2::Op>& Ops() const { return m_ops; }

    using StateHash = std::array<unsigned char, 32>;

    /**
     * Embed one token sequence (ending with EOS). Returns the int8 direction
     * vector (hidden_size). If trace is set, every weight-matmul input is
     * reported in forward order. If states is set, it receives the
     * num_hidden_layers + 1 state hashes committed in results (fraud proofs,
     * doc/pouw-v2.md): HashState(0, x_0) after the embedding lookup and
     * HashState(l, x_l) after layer l.
     */
    std::vector<int8_t> Embed(const std::vector<uint32_t>& ids, std::vector<OpTrace>* trace = nullptr,
                              std::vector<StateHash>* states = nullptr) const;

    /** The forward pass as single steps (fraud-proof granularity). x is T x hidden_size, Q16. */
    bool ValidInput(const std::vector<uint32_t>& ids) const;
    std::vector<int64_t> Tokens(const std::vector<uint32_t>& ids) const;
    void Layer(uint32_t l, std::vector<int64_t>& x, size_t T, std::vector<OpTrace>* trace = nullptr) const;
    std::vector<int8_t> Final(std::vector<int64_t> x, size_t T) const;
    /** BLAKE3("ApertureState/v0" || index u32 LE || x as int64 LE). */
    static StateHash HashState(uint32_t index, const std::vector<int64_t>& x);

    /** Byte tokenizer (tiny/regtest model): UTF-8 bytes then EOS. */
    std::vector<uint32_t> TokenizeBytes(const std::string& text) const;

private:
    std::unique_ptr<apm::Model> m_model;
    std::vector<matmulpow_v2::Op> m_ops;
    std::vector<const apm::Tensor*> m_scales;
    int64_t m_isq{0};
};

/** Build the deterministic tiny model image (doc/protocol-model.md, regtest). */
std::vector<unsigned char> BuildTinyModel(uint32_t seed);

} // namespace intmodel

#endif // BITCOIN_MODEL_INTMODEL_H
