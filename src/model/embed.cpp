// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <model/embed.h>

#include <consensus/params.h>
#include <crypto/blake3/blake3.h>
#include <crypto/common.h>
#include <crypto/matmulpow_v2.h>
#include <primitives/block.h>

#include <map>
#include <mutex>

namespace embed {
namespace {

const unsigned char REQ_TAG[4] = {'A', 'P', 'E', 'R'};
const unsigned char RES_TAG[4] = {'A', 'P', 'E', 'M'};
const unsigned char FP_TAG[4] = {'A', 'P', 'F', 'P'};

std::mutex g_model_mutex;
std::unique_ptr<intmodel::IntModel> g_model;

std::mutex g_cache_mutex;
struct CacheEntry {
    std::vector<int8_t> embedding;
    std::vector<intmodel::IntModel::StateHash> states;
};
std::map<uint256, CacheEntry> g_cache;
constexpr size_t CACHE_MAX = 4096;

/** Parse OP_RETURN <tag> <push>...; returns the concatenated data after tag. */
bool ParseTagged(const CScript& script, const unsigned char tag[4], std::vector<std::vector<unsigned char>>& pushes)
{
    CScript::const_iterator pc = script.begin();
    opcodetype op;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, op) || op != OP_RETURN) return false;
    if (!script.GetOp(pc, op, data) || data.size() != 4 || memcmp(data.data(), tag, 4) != 0) return false;
    pushes.clear();
    while (pc < script.end()) {
        if (!script.GetOp(pc, op, data) || op > OP_PUSHDATA4) return false;
        pushes.push_back(data);
    }
    return true;
}

void AppendChunks(CScript& s, const unsigned char* data, size_t len)
{
    for (size_t off = 0; off < len; off += MAX_PUSH) {
        const size_t n = std::min<size_t>(MAX_PUSH, len - off);
        s << std::vector<unsigned char>(data + off, data + off + n);
    }
}

} // namespace

bool IsRequestScript(const CScript& script)
{
    return script.size() >= 6 && script[0] == OP_RETURN && script[1] == 4 && memcmp(&script[2], REQ_TAG, 4) == 0;
}

bool ParseRequest(const CScript& script, std::vector<uint32_t>& ids)
{
    std::vector<std::vector<unsigned char>> pushes;
    if (!ParseTagged(script, REQ_TAG, pushes)) return false;
    std::vector<unsigned char> all;
    for (const auto& p : pushes) all.insert(all.end(), p.begin(), p.end());
    if (all.size() % 3 != 0) return false;
    ids.clear();
    for (size_t k = 0; k < all.size(); k += 3) ids.push_back(all[k] | (all[k + 1] << 8) | (uint32_t{all[k + 2]} << 16));
    return true;
}

CScript MakeRequestScript(const std::vector<uint32_t>& ids)
{
    std::vector<unsigned char> raw;
    for (uint32_t id : ids) {
        raw.push_back(id & 0xff);
        raw.push_back((id >> 8) & 0xff);
        raw.push_back((id >> 16) & 0xff);
    }
    CScript s;
    s << OP_RETURN << std::vector<unsigned char>(REQ_TAG, REQ_TAG + 4);
    AppendChunks(s, raw.data(), raw.size());
    return s;
}

bool ParseResult(const CScript& script, Result& out)
{
    std::vector<std::vector<unsigned char>> pushes;
    if (!ParseTagged(script, RES_TAG, pushes) || pushes.size() < 3) return false;
    if (pushes[0].size() != 32 || pushes[1].size() != 4) return false;
    out.outpoint = COutPoint(uint256(pushes[0]), ReadLE32(pushes[1].data()));
    std::vector<unsigned char> body;
    for (size_t k = 2; k < pushes.size(); ++k) body.insert(body.end(), pushes[k].begin(), pushes[k].end());
    if (body.size() < 2) return false;
    const size_t n = body[0] | (body[1] << 8);
    if (body.size() < 2 + 32 * n) return false;
    out.states.assign(n, {});
    for (size_t k = 0; k < n; ++k) std::copy(body.begin() + 2 + 32 * k, body.begin() + 2 + 32 * (k + 1), out.states[k].begin());
    out.embedding.clear();
    for (size_t k = 2 + 32 * n; k < body.size(); ++k) out.embedding.push_back(static_cast<int8_t>(body[k]));
    return true;
}

CScript MakeResultScript(const Result& r)
{
    CScript s;
    unsigned char vout[4];
    WriteLE32(vout, r.outpoint.n);
    s << OP_RETURN << std::vector<unsigned char>(RES_TAG, RES_TAG + 4)
      << std::vector<unsigned char>(r.outpoint.hash.begin(), r.outpoint.hash.end())
      << std::vector<unsigned char>(vout, vout + 4);
    std::vector<unsigned char> body{static_cast<unsigned char>(r.states.size() & 0xff), static_cast<unsigned char>(r.states.size() >> 8)};
    for (const auto& h : r.states) body.insert(body.end(), h.begin(), h.end());
    for (int8_t v : r.embedding) body.push_back(static_cast<unsigned char>(v));
    AppendChunks(s, body.data(), body.size());
    return s;
}

bool ParseFraudProof(const CScript& script, FraudProof& out)
{
    std::vector<std::vector<unsigned char>> pushes;
    if (!ParseTagged(script, FP_TAG, pushes) || pushes.size() < 2) return false;
    if (pushes[0].size() != 2 || pushes[1].size() != 1) return false;
    out.result_index = pushes[0][0] | (pushes[0][1] << 8);
    out.step = pushes[1][0];
    std::vector<unsigned char> raw;
    for (size_t k = 2; k < pushes.size(); ++k) raw.insert(raw.end(), pushes[k].begin(), pushes[k].end());
    if (raw.size() % 8 != 0) return false;
    out.state.resize(raw.size() / 8);
    for (size_t k = 0; k < out.state.size(); ++k) out.state[k] = static_cast<int64_t>(ReadLE64(raw.data() + 8 * k));
    return true;
}

CScript MakeFraudProofScript(const FraudProof& p)
{
    CScript s;
    s << OP_RETURN << std::vector<unsigned char>(FP_TAG, FP_TAG + 4)
      << std::vector<unsigned char>{static_cast<unsigned char>(p.result_index & 0xff), static_cast<unsigned char>(p.result_index >> 8)}
      << std::vector<unsigned char>{p.step};
    std::vector<unsigned char> raw(8 * p.state.size());
    for (size_t k = 0; k < p.state.size(); ++k) WriteLE64(raw.data() + 8 * k, static_cast<uint64_t>(p.state[k]));
    AppendChunks(s, raw.data(), raw.size());
    return s;
}

bool VerifyFraudProof(const intmodel::IntModel& model, const std::vector<uint32_t>& input, const Result& result,
                      const FraudProof& proof, std::string& why)
{
    using IM = intmodel::IntModel;
    const uint32_t L = model.Config().num_hidden_layers;
    const size_t T = input.size(), H = model.Config().hidden_size;
    if (!model.ValidInput(input)) {
        why = "request cannot be embedded";
        return false;
    }
    // A structurally invalid result cannot be in a valid block; still refuse.
    if (result.states.size() != L + 1 || result.embedding.size() != H) {
        why = "malformed result";
        return false;
    }
    if (proof.step == 0) {
        if (!proof.state.empty()) {
            why = "step 0 takes no state";
            return false;
        }
        if (IM::HashState(0, model.Tokens(input)) != result.states[0]) return true;
        why = "embedding lookup matches the commitment";
        return false;
    }
    if (proof.step > L + 1) {
        why = "step out of range";
        return false;
    }
    if (proof.state.size() != T * H) {
        why = "state has the wrong size";
        return false;
    }
    // The supplied state must be the one the miner committed as input to this step.
    if (IM::HashState(proof.step - 1, proof.state) != result.states[proof.step - 1]) {
        why = "state does not match the committed input";
        return false;
    }
    if (proof.step <= L) {
        std::vector<int64_t> x = proof.state;
        model.Layer(proof.step - 1, x, T);
        if (IM::HashState(proof.step, x) != result.states[proof.step]) return true;
        why = "layer output matches the commitment";
        return false;
    }
    if (model.Final(proof.state, T) != result.embedding) return true;
    why = "final embedding matches the result";
    return false;
}

bool BuildFraudProof(const intmodel::IntModel& model, const std::vector<uint32_t>& input, const Result& result,
                     uint16_t result_index, FraudProof& out)
{
    using IM = intmodel::IntModel;
    const uint32_t L = model.Config().num_hidden_layers;
    if (!model.ValidInput(input) || result.states.size() != L + 1) return false;
    const size_t T = input.size();
    out = FraudProof{};
    out.result_index = result_index;
    std::vector<int64_t> x = model.Tokens(input);
    if (IM::HashState(0, x) != result.states[0]) {
        out.step = 0;
        return true;
    }
    for (uint32_t l = 0; l < L; ++l) {
        std::vector<int64_t> next = x;
        model.Layer(l, next, T);
        if (IM::HashState(l + 1, next) != result.states[l + 1]) {
            out.step = static_cast<uint8_t>(l + 1);
            out.state = std::move(x);
            return true;
        }
        x = std::move(next);
    }
    if (model.Final(x, T) != result.embedding) {
        out.step = static_cast<uint8_t>(L + 1);
        out.state = std::move(x);
        return true;
    }
    return false;
}

std::vector<Request> CollectRequests(const CBlock& block)
{
    std::vector<Request> out;
    for (size_t t = 1; t < block.vtx.size(); ++t) {
        const CTransaction& tx = *block.vtx[t];
        for (uint32_t n = 0; n < tx.vout.size(); ++n) {
            if (!IsRequestScript(tx.vout[n].scriptPubKey)) continue;
            Request r;
            r.outpoint = COutPoint(tx.GetHash(), n);
            if (!ParseRequest(tx.vout[n].scriptPubKey, r.ids)) r.ids.assign(1, 0xffffffff); // malformed: rejected by ModelInput
            out.push_back(std::move(r));
        }
    }
    return out;
}

uint256 BatchRoot(const std::vector<Request>& requests)
{
    static const char TAG[] = "ApertureBatch/v0";
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, TAG, sizeof(TAG) - 1);
    for (const Request& r : requests) {
        unsigned char vout[4];
        WriteLE32(vout, r.outpoint.n);
        blake3_hasher_update(&h, r.outpoint.hash.begin(), 32);
        blake3_hasher_update(&h, vout, 4);
    }
    uint256 out;
    blake3_hasher_finalize(&h, out.begin(), 32);
    return out;
}

std::vector<uint32_t> ModelInput(const intmodel::IntModel& model, const std::vector<uint32_t>& ids)
{
    const apm::Config& c = model.Config();
    if (ids.size() + 1 > c.max_positions) return {};
    std::vector<uint32_t> in;
    for (uint32_t id : ids) {
        if (id >= c.vocab_size) return {};
        in.push_back(id);
    }
    in.push_back(c.eos_token_id);
    return in;
}

const intmodel::IntModel* GetProtocolModel()
{
    std::lock_guard<std::mutex> lock(g_model_mutex);
    return g_model.get();
}

bool LoadProtocolModel(const std::string& path, const std::string& expected_model_id, unsigned int rank, bool allow_override, std::string& error)
{
    auto apm_model = std::make_unique<apm::Model>();
    if (!path.empty()) {
        if (!apm_model->LoadFile(path, error)) return false;
    } else if (!apm_model->Load(intmodel::BuildTinyModel(1), error)) {
        return false;
    }
    if (apm_model->ModelIdHex() != expected_model_id && !allow_override) {
        error = "protocol model id " + apm_model->ModelIdHex() + " does not match the chain's " + expected_model_id;
        return false;
    }
    auto model = std::make_unique<intmodel::IntModel>();
    if (!model->Init(std::move(apm_model), error)) return false;
    std::lock_guard<std::mutex> lock(g_model_mutex);
    matmulpow_v2::SetModel(model->Ops(), rank);
    g_model = std::move(model);
    {
        std::lock_guard<std::mutex> cl(g_cache_mutex);
        g_cache.clear();
    }
    return true;
}

void UnloadProtocolModel()
{
    std::lock_guard<std::mutex> lock(g_model_mutex);
    matmulpow_v2::SetModel({}, 0);
    g_model.reset();
}

std::vector<int8_t> Embed(const intmodel::IntModel& model, const std::vector<uint32_t>& input, std::vector<intmodel::OpTrace>* trace,
                          std::vector<intmodel::IntModel::StateHash>* states)
{
    blake3_hasher h;
    blake3_hasher_init(&h);
    for (uint32_t id : input) {
        unsigned char b[4];
        WriteLE32(b, id);
        blake3_hasher_update(&h, b, 4);
    }
    uint256 key;
    blake3_hasher_finalize(&h, key.begin(), 32);
    if (!trace) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        const auto it = g_cache.find(key);
        if (it != g_cache.end()) {
            if (states) *states = it->second.states;
            return it->second.embedding;
        }
    }
    CacheEntry entry;
    entry.embedding = model.Embed(input, trace, &entry.states);
    if (states) *states = entry.states;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (g_cache.size() >= CACHE_MAX) g_cache.clear();
    g_cache[key] = entry;
    return entry.embedding;
}

std::string CheckBlockEmbeddings(const CBlock& block, const Consensus::Params& params)
{
    const intmodel::IntModel* model = GetProtocolModel();
    if (!model) return "powv2-no-model";
    const std::vector<Request> requests = CollectRequests(block);
    if (requests.size() > params.nMaxEmbedRequests) return "bad-embed-count";
    if (BatchRoot(requests) != block.powv2.batch_root) return "bad-powv2-batchroot";
    std::vector<Result> results;
    for (const CTxOut& out : block.vtx[0]->vout) {
        Result r;
        if (ParseResult(out.scriptPubKey, r)) results.push_back(std::move(r));
    }
    if (results.size() != requests.size()) return "bad-embed-result-count";
    const size_t n_states = model->Config().num_hidden_layers + 1, H = model->Config().hidden_size;
    uint64_t tokens = 0;
    for (size_t k = 0; k < requests.size(); ++k) {
        const std::vector<uint32_t> input = ModelInput(*model, requests[k].ids);
        if (input.empty()) return "bad-embed-request";
        tokens += input.size();
        if (tokens > params.nMaxEmbedTokens) return "bad-embed-tokens";
        if (results[k].outpoint != requests[k].outpoint) return "bad-embed-result-order";
        if (results[k].states.size() != n_states || results[k].embedding.size() != H) return "bad-embed-result-format";
        // Optimistic chains leave correctness to fraud claims (CheckFraudClaim).
        if (params.fPowV2Optimistic) continue;
        std::vector<intmodel::IntModel::StateHash> states;
        if (Embed(*model, input, nullptr, &states) != results[k].embedding || states != results[k].states) return "bad-embed-result";
    }
    return "";
}

} // namespace embed
