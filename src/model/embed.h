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
 *   result:  OP_RETURN "APEM" <request txid> <vout u32 LE> <embedding int8, <= 520-byte pushes>
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
    std::vector<int8_t> embedding;
};

bool IsRequestScript(const CScript& script);
bool ParseRequest(const CScript& script, std::vector<uint32_t>& ids);
CScript MakeRequestScript(const std::vector<uint32_t>& ids);
bool ParseResult(const CScript& script, Result& out);
CScript MakeResultScript(const Result& result);

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
std::vector<int8_t> Embed(const intmodel::IntModel& model, const std::vector<uint32_t>& input, std::vector<intmodel::OpTrace>* trace = nullptr);

/**
 * Validate the v2 block body: well-formed requests, batch_root, and exactly
 * one correct coinbase result per request in order. Returns an empty string
 * on success, otherwise a reject reason.
 */
std::string CheckBlockEmbeddings(const CBlock& block, const Consensus::Params& params);

} // namespace embed

#endif // BITCOIN_MODEL_EMBED_H
