// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MODEL_EMBED_H
#define BITCOIN_MODEL_EMBED_H

#include <model/intmodel.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <memory>
#include <string>
#include <vector>

class CBlock;
namespace Consensus { struct Params; }

/**
 * Embedding requests and results (doc/pouw-v2.md, "Block body").
 *
 *   request: OP_RETURN "APER" <token ids as u24 LE, split in <= 520-byte pushes>
 *   result:  OP_RETURN "APEM" <request txid> <vout u32 LE> <body in <= 520-byte pushes>
 *            body = state_count u16 LE || state hashes (32 bytes each) || embedding int8
 *   claim:   OP_RETURN "APFP" <result index u16 LE> <step u8> <state int64 LE, <= 520-byte pushes>
 *
 * The state hashes are the per-layer forward-pass commitments that make
 * fraud proofs cheap: a claim re-executes one step (embedding lookup, one
 * layer, or the final norm) instead of the whole model.
 *
 * A request is identified by its outpoint. Every request in a v2 block must be
 * served by a result in that block's coinbase, in block order, and the
 * header's batch_root commits to the ordered request list.
 */
namespace embed {

static constexpr unsigned int MAX_PUSH = 520;

struct Request {
    COutPoint outpoint;
    std::vector<uint32_t> ids; //!< without the protocol-appended EOS
};

struct Result {
    COutPoint outpoint;
    std::vector<intmodel::IntModel::StateHash> states; //!< num_hidden_layers + 1
    std::vector<int8_t> embedding;
};

/**
 * Fraud claim for one result of a block. step 0: the embedding lookup
 * disagrees with states[0]; step l in 1..L: layer l applied to `state`
 * (which matches states[l-1]) disagrees with states[l]; step L+1: the final
 * norm of `state` (which matches states[L]) disagrees with the embedding.
 */
struct FraudProof {
    uint16_t result_index{0};
    uint8_t step{0};
    std::vector<int64_t> state;
};

bool IsRequestScript(const CScript& script);
bool ParseRequest(const CScript& script, std::vector<uint32_t>& ids);
CScript MakeRequestScript(const std::vector<uint32_t>& ids);
bool ParseResult(const CScript& script, Result& out);
CScript MakeResultScript(const Result& result);

bool ParseFraudProof(const CScript& script, FraudProof& out);
CScript MakeFraudProofScript(const FraudProof& proof);

/**
 * Re-execute the claimed step. Returns true if it proves the result wrong;
 * otherwise `why` says why the claim fails.
 */
bool VerifyFraudProof(const intmodel::IntModel& model, const std::vector<uint32_t>& input, const Result& result,
                      const FraudProof& proof, std::string& why);

/** Find the first wrong commitment of a result; false if the result is correct. */
bool BuildFraudProof(const intmodel::IntModel& model, const std::vector<uint32_t>& input, const Result& result,
                     uint16_t result_index, FraudProof& out);

/** All request outputs of non-coinbase transactions, in block order. */
std::vector<Request> CollectRequests(const CBlock& block);
/** BLAKE3("ApertureBatch/v0" || txid || vout u32 LE ...). */
uint256 BatchRoot(const std::vector<Request>& requests);

/** Token sequence the model runs for a request (ids + EOS), empty if invalid. */
std::vector<uint32_t> ModelInput(const intmodel::IntModel& model, const std::vector<uint32_t>& ids);

/** The loaded protocol model, or nullptr. */
const intmodel::IntModel* GetProtocolModel();
/**
 * Load the protocol model for the active chain: `path` if non-empty, otherwise
 * the built-in tiny model when `expected_model_id` names it. Registers the
 * PoW ops with matmulpow_v2. Returns false with an error message on failure.
 */
bool LoadProtocolModel(const std::string& path, const std::string& expected_model_id, unsigned int rank, bool allow_override, std::string& error);
void UnloadProtocolModel();

/** Embed with a small cache (blocks are validated more than once). */
std::vector<int8_t> Embed(const intmodel::IntModel& model, const std::vector<uint32_t>& input, std::vector<intmodel::OpTrace>* trace = nullptr,
                          std::vector<intmodel::IntModel::StateHash>* states = nullptr);

/**
 * Validate the v2 block body: well-formed requests, batch_root, and exactly
 * one well-formed coinbase result per request in order. Unless the chain is
 * optimistic (Consensus::Params::fPowV2Optimistic), every result is also
 * recomputed. Returns an empty string on success, otherwise a reject reason.
 */
std::string CheckBlockEmbeddings(const CBlock& block, const Consensus::Params& params);

} // namespace embed

#endif // BITCOIN_MODEL_EMBED_H
