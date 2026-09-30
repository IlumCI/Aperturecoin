// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MODEL_INTMODEL_H
#define BITCOIN_MODEL_INTMODEL_H

#include <crypto/matmulpow_v2.h>
#include <model/apm.h>

#include <stdint.h>

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

    /**
     * Embed one token sequence (ending with EOS). Returns the int8 direction
     * vector (hidden_size). If trace is set, every weight-matmul input is
     * reported in forward order.
     */
    std::vector<int8_t> Embed(const std::vector<uint32_t>& ids, std::vector<OpTrace>* trace = nullptr) const;

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
