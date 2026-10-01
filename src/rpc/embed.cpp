// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <crypto/matmulpow_v2.h>
#include <model/embed.h>
#include <primitives/block.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <util/strencodings.h>
#include <validation.h>
#include <core_io.h>
#include <key_io.h>
#include <script/standard.h>
#include <rpc/rawtransaction_util.h>

#include <cmath>

static std::vector<uint32_t> ParseEmbedInput(const UniValue& v)
{
    const intmodel::IntModel* model = embed::GetProtocolModel();
    if (!model) throw JSONRPCError(RPC_MISC_ERROR, "No protocol model loaded (ApertureMatMul v2 inactive)");
    std::vector<uint32_t> ids;
    UniValue parsed;
    if (v.isStr() && !v.get_str().empty() && v.get_str()[0] == '[' && parsed.read(v.get_str()) && parsed.isArray()) {
        return ParseEmbedInput(parsed);
    }
    if (v.isStr()) {
        if (model->Config().tokenizer != "bytes") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "This model needs token ids (tokenize with its tokenizer, see contrib/aperture-model)");
        }
        for (unsigned char c : v.get_str()) ids.push_back(c);
    } else if (v.isArray()) {
        for (size_t k = 0; k < v.size(); ++k) {
            const int64_t id = v[k].get_int64();
            if (id < 0 || id > 0xffffff) throw JSONRPCError(RPC_INVALID_PARAMETER, "token id out of range");
            ids.push_back(static_cast<uint32_t>(id));
        }
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "input must be a string or an array of token ids");
    }
    if (embed::ModelInput(*model, ids).empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("input must have at most %u tokens and ids below %u",
                                                            model->Config().max_positions - 1, model->Config().vocab_size));
    }
    return ids;
}

static std::vector<int8_t> EmbedIds(const std::vector<uint32_t>& ids, std::vector<intmodel::IntModel::StateHash>* states = nullptr)
{
    const intmodel::IntModel* model = embed::GetProtocolModel();
    return embed::Embed(*model, embed::ModelInput(*model, ids), nullptr, states);
}

static UniValue StatesJson(const std::vector<intmodel::IntModel::StateHash>& states)
{
    UniValue arr(UniValue::VARR);
    for (const auto& h : states) arr.push_back(HexStr(Span<const unsigned char>(h.data(), h.size())));
    return arr;
}

static std::string I8Hex(const std::vector<int8_t>& v)
{
    return HexStr(Span<const unsigned char>(reinterpret_cast<const unsigned char*>(v.data()), v.size()));
}

static double Cosine(const std::vector<int8_t>& a, const std::vector<int8_t>& b)
{
    double dot = 0, na = 0, nb = 0;
    for (size_t k = 0; k < a.size() && k < b.size(); ++k) {
        dot += double(a[k]) * b[k];
        na += double(a[k]) * a[k];
        nb += double(b[k]) * b[k];
    }
    return na > 0 && nb > 0 ? dot / std::sqrt(na * nb) : 0.0;
}

static const RPCArg INPUT_ARG{"input", RPCArg::Type::STR, RPCArg::Optional::NO,
                              "Text (byte-tokenizer models) or a JSON array of token ids"};

static RPCHelpMan embed_rpc()
{
    return RPCHelpMan{"embed",
        "\nCompute the protocol-model embedding of an input locally, exactly as miners must (doc/protocol-model.md).\n",
        {INPUT_ARG},
        RPCResult{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "model_id", "Protocol model id"},
                {RPCResult::Type::NUM, "tokens", "Tokens run (including EOS)"},
                {RPCResult::Type::STR_HEX, "embedding", "int8 embedding"},
                {RPCResult::Type::ARR, "state_hashes", "per-layer state commitments of a result", {{RPCResult::Type::STR_HEX, "", ""}}},
            }},
        RPCExamples{HelpExampleCli("embed", "\"hello world\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::vector<uint32_t> ids = ParseEmbedInput(request.params[0]);
    std::vector<intmodel::IntModel::StateHash> states;
    const std::vector<int8_t> e = EmbedIds(ids, &states);
    UniValue out(UniValue::VOBJ);
    out.pushKV("model_id", embed::GetProtocolModel()->Apm().ModelIdHex());
    out.pushKV("tokens", (uint64_t)ids.size() + 1);
    out.pushKV("embedding", I8Hex(e));
    out.pushKV("state_hashes", StatesJson(states));
    return out;
},
    };
}

static RPCHelpMan createembeddingrequest()
{
    return RPCHelpMan{"createembeddingrequest",
        "\nBuild the scriptPubKey of an embedding request output (doc/pouw-v2.md).\n",
        {INPUT_ARG},
        RPCResult{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "script", "OP_RETURN \"APER\" <token ids> scriptPubKey"},
                {RPCResult::Type::NUM, "tokens", "Token ids in the request"},
            }},
        RPCExamples{HelpExampleCli("createembeddingrequest", "\"hello world\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::vector<uint32_t> ids = ParseEmbedInput(request.params[0]);
    const CScript s = embed::MakeRequestScript(ids);
    UniValue out(UniValue::VOBJ);
    out.pushKV("script", HexStr(s));
    out.pushKV("tokens", (uint64_t)ids.size());
    return out;
},
    };
}

static void BlockResults(const CBlock& block, UniValue& arr, const CBlockIndex* pindex)
{
    const std::vector<embed::Request> requests = embed::CollectRequests(block);
    std::map<COutPoint, std::vector<int8_t>> results;
    for (const CTxOut& o : block.vtx[0]->vout) {
        embed::Result r;
        if (embed::ParseResult(o.scriptPubKey, r)) results[r.outpoint] = r.embedding;
    }
    for (const embed::Request& r : requests) {
        UniValue e(UniValue::VOBJ);
        e.pushKV("txid", r.outpoint.hash.GetHex());
        e.pushKV("vout", (uint64_t)r.outpoint.n);
        e.pushKV("tokens", (uint64_t)r.ids.size());
        const auto it = results.find(r.outpoint);
        e.pushKV("embedding", it == results.end() ? "" : I8Hex(it->second));
        if (pindex) {
            e.pushKV("blockhash", pindex->GetBlockHash().GetHex());
            e.pushKV("height", pindex->nHeight);
        }
        arr.push_back(e);
    }
}

static RPCHelpMan getblockembeddings()
{
    return RPCHelpMan{"getblockembeddings",
        "\nList the embedding requests served in a block and their results.\n",
        {{"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Block hash"}},
        RPCResult{RPCResult::Type::ARR, "", "",
            {{RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "txid", "Request transaction"},
                    {RPCResult::Type::NUM, "vout", "Request output"},
                    {RPCResult::Type::NUM, "tokens", "Token ids"},
                    {RPCResult::Type::STR_HEX, "embedding", "int8 result"},
                }}}},
        RPCExamples{HelpExampleCli("getblockembeddings", "\"<blockhash>\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const uint256 hash(ParseHashV(request.params[0], "blockhash"));
    CBlock block;
    {
        LOCK(cs_main);
        const CBlockIndex* pindex = LookupBlockIndex(hash);
        if (!pindex) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) throw JSONRPCError(RPC_MISC_ERROR, "Block not available");
    }
    UniValue arr(UniValue::VARR);
    BlockResults(block, arr, nullptr);
    return arr;
},
    };
}

static RPCHelpMan searchembeddings()
{
    return RPCHelpMan{"searchembeddings",
        "\nSemantic search over the embeddings miners produced in recent blocks: the query is embedded\n"
        "locally and ranked against every served request by cosine similarity.\n",
        {
            INPUT_ARG,
            {"blocks", RPCArg::Type::NUM, /* default */ "1000", "How many recent blocks to search"},
            {"count", RPCArg::Type::NUM, /* default */ "5", "Results to return"},
        },
        RPCResult{RPCResult::Type::ARR, "", "",
            {{RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "score", "Cosine similarity"},
                    {RPCResult::Type::STR_HEX, "txid", "Request transaction"},
                    {RPCResult::Type::NUM, "vout", "Request output"},
                    {RPCResult::Type::STR_HEX, "blockhash", "Block"},
                    {RPCResult::Type::NUM, "height", "Height"},
                }}}},
        RPCExamples{HelpExampleCli("searchembeddings", "\"query text\" 100 3")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::vector<int8_t> q = EmbedIds(ParseEmbedInput(request.params[0]));
    const int blocks = request.params[1].isNull() ? 1000 : request.params[1].get_int();
    const size_t count = request.params[2].isNull() ? 5 : request.params[2].get_int();
    UniValue all(UniValue::VARR);
    {
        LOCK(cs_main);
        const CBlockIndex* pindex = ::ChainActive().Tip();
        for (int k = 0; pindex && k < blocks; ++k, pindex = pindex->pprev) {
            if (!(pindex->nVersion & CBlockHeader::VERSION_POWV2)) break;
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) continue;
            BlockResults(block, all, pindex);
        }
    }
    std::vector<std::pair<double, size_t>> scored;
    for (size_t k = 0; k < all.size(); ++k) {
        const std::vector<unsigned char> e = ParseHex(all[k]["embedding"].get_str());
        scored.emplace_back(Cosine(q, std::vector<int8_t>(e.begin(), e.end())), k);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    UniValue out(UniValue::VARR);
    for (size_t k = 0; k < scored.size() && k < count; ++k) {
        const UniValue& e = all[scored[k].second];
        UniValue o(UniValue::VOBJ);
        o.pushKV("score", scored[k].first);
        o.pushKV("txid", e["txid"]);
        o.pushKV("vout", e["vout"]);
        o.pushKV("blockhash", e["blockhash"]);
        o.pushKV("height", e["height"]);
        out.push_back(o);
    }
    return out;
},
    };
}

static bool ReadBlockByHash(const uint256& hash, CBlock& block, int& height)
{
    LOCK(cs_main);
    const CBlockIndex* pindex = LookupBlockIndex(hash);
    if (!pindex) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
    if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) throw JSONRPCError(RPC_MISC_ERROR, "Block not available");
    height = pindex->nHeight;
    return ::ChainActive().Contains(pindex);
}

static std::vector<embed::Result> BlockResultList(const CBlock& block)
{
    std::vector<embed::Result> results;
    for (const CTxOut& out : block.vtx[0]->vout) {
        embed::Result r;
        if (embed::ParseResult(out.scriptPubKey, r)) results.push_back(std::move(r));
    }
    return results;
}

static RPCHelpMan checkblockembeddings()
{
    return RPCHelpMan{"checkblockembeddings",
        "\nRecompute every embedding result of a block with the local protocol model and report wrong ones.\n"
        "On an optimistic chain (-powv2optimistic) wrong results are valid until a fraud claim proves them;\n"
        "see createfraudclaim.\n",
        {{"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Block hash"}},
        RPCResult{RPCResult::Type::ARR, "", "",
            {{RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "index", "Result index"},
                    {RPCResult::Type::STR_HEX, "txid", "Request transaction"},
                    {RPCResult::Type::BOOL, "valid", "Whether the result and all its state commitments are correct"},
                    {RPCResult::Type::NUM, "fraud_step", /* optional */ true, "First wrong step: 0 lookup, 1..L layer, L+1 final"},
                }}}},
        RPCExamples{HelpExampleCli("checkblockembeddings", "\"<blockhash>\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const intmodel::IntModel* model = embed::GetProtocolModel();
    if (!model) throw JSONRPCError(RPC_MISC_ERROR, "No protocol model loaded");
    CBlock block;
    int height;
    ReadBlockByHash(ParseHashV(request.params[0], "blockhash"), block, height);
    const std::vector<embed::Request> requests = embed::CollectRequests(block);
    const std::vector<embed::Result> results = BlockResultList(block);
    UniValue out(UniValue::VARR);
    for (size_t k = 0; k < results.size() && k < requests.size(); ++k) {
        UniValue o(UniValue::VOBJ);
        o.pushKV("index", (uint64_t)k);
        o.pushKV("txid", requests[k].outpoint.hash.GetHex());
        embed::FraudProof proof;
        const bool fraud = embed::BuildFraudProof(*model, embed::ModelInput(*model, requests[k].ids), results[k], static_cast<uint16_t>(k), proof);
        o.pushKV("valid", !fraud);
        if (fraud) o.pushKV("fraud_step", (int)proof.step);
        out.push_back(o);
    }
    return out;
},
    };
}

static RPCHelpMan createfraudclaim()
{
    return RPCHelpMan{"createfraudclaim",
        "\nBuild a fraud claim proving an embedding result of a block wrong (doc/pouw-v2.md, \"Fraud proofs\").\n"
        "The claim re-executes one forward-pass step and takes every coinbase output of that block except the\n"
        "development fund; it needs no signatures. Broadcast it with sendrawtransaction before the miner can\n"
        "spend the coinbase (coinbase maturity).\n",
        {
            {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Block with the wrong result"},
            {"index", RPCArg::Type::NUM, RPCArg::Optional::NO, "Result index (see checkblockembeddings)"},
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "Where the forfeited coinbase goes"},
            {"fee", RPCArg::Type::AMOUNT, /* default */ "0.001", "Fee paid by the claim"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "hex", "Claim transaction"},
                {RPCResult::Type::NUM, "step", "Re-executed step"},
                {RPCResult::Type::STR_AMOUNT, "amount", "Forfeited amount paid to the address"},
            }},
        RPCExamples{HelpExampleCli("createfraudclaim", "\"<blockhash>\" 0 \"<address>\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const intmodel::IntModel* model = embed::GetProtocolModel();
    if (!model) throw JSONRPCError(RPC_MISC_ERROR, "No protocol model loaded");
    CBlock block;
    int height;
    if (!ReadBlockByHash(ParseHashV(request.params[0], "blockhash"), block, height)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Block is not in the active chain");
    }
    const size_t index = request.params[1].get_int();
    const CTxDestination dest = DecodeDestination(request.params[2].get_str());
    if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    const CAmount fee = request.params[3].isNull() ? COIN / 1000 : AmountFromValue(request.params[3]);
    const std::vector<embed::Request> requests = embed::CollectRequests(block);
    const std::vector<embed::Result> results = BlockResultList(block);
    if (index >= results.size() || index >= requests.size()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Result index out of range");
    embed::FraudProof proof;
    if (!embed::BuildFraudProof(*model, embed::ModelInput(*model, requests[index].ids), results[index], static_cast<uint16_t>(index), proof)) {
        throw JSONRPCError(RPC_VERIFY_REJECTED, "The result is correct; there is no fraud to prove");
    }
    const Consensus::Params& params = Params().GetConsensus();
    const CScript devfund(params.devFundScript.begin(), params.devFundScript.end());
    CMutableTransaction tx;
    tx.nVersion = 2;
    CAmount total = 0;
    const CTransaction& cb = *block.vtx[0];
    for (uint32_t n = 0; n < cb.vout.size(); ++n) {
        if (cb.vout[n].nValue > 0 && cb.vout[n].scriptPubKey != devfund) {
            tx.vin.emplace_back(COutPoint(cb.GetHash(), n));
            total += cb.vout[n].nValue;
        }
    }
    if (total <= fee) throw JSONRPCError(RPC_VERIFY_REJECTED, "Forfeitable amount does not cover the fee");
    tx.vout.emplace_back(total - fee, GetScriptForDestination(dest));
    tx.vout.emplace_back(0, embed::MakeFraudProofScript(proof));
    UniValue out(UniValue::VOBJ);
    out.pushKV("hex", EncodeHexTx(CTransaction(tx)));
    out.pushKV("step", (int)proof.step);
    out.pushKV("amount", ValueFromAmount(total - fee));
    return out;
},
    };
}

static RPCHelpMan estimaterequestfee()
{
    return RPCHelpMan{"estimaterequestfee",
        "\nConsensus minimum fee for an embedding request: tokens (EOS included) x the chain's minimum fee per token.\n"
        "The transaction also pays its ordinary size-based fee; the larger of the two applies.\n",
        {INPUT_ARG},
        RPCResult{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "tokens", "Model input tokens, EOS included"},
                {RPCResult::Type::STR_AMOUNT, "fee_per_token", "Consensus minimum per token"},
                {RPCResult::Type::STR_AMOUNT, "min_fee", "Consensus minimum for this request"},
                {RPCResult::Type::NUM, "block_token_budget", "Tokens a block can serve"},
            }},
        RPCExamples{HelpExampleCli("estimaterequestfee", "\"[9707, 1879]\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::vector<uint32_t> ids = ParseEmbedInput(request.params[0]);
    const Consensus::Params& params = Params().GetConsensus();
    const uint64_t tokens = ids.size() + 1;
    UniValue out(UniValue::VOBJ);
    out.pushKV("tokens", tokens);
    out.pushKV("fee_per_token", ValueFromAmount(params.nMinRequestFeePerToken));
    out.pushKV("min_fee", ValueFromAmount(static_cast<CAmount>(tokens) * params.nMinRequestFeePerToken));
    out.pushKV("block_token_budget", (uint64_t)params.nMaxEmbedTokens);
    return out;
},
    };
}

static RPCHelpMan getusefulshare()
{
    return RPCHelpMan{"getusefulshare",
        "\nUseful share of ApertureMatMul v2 mining over a window of blocks (doc/pouw-v2.md, \"Usefulness accounting\").\n"
        "Each block costs one clean forward pass over its batch plus the ticket search. The search re-noises the\n"
        "same products for every nonce, so it adds work without adding results. The useful share is the\n"
        "weight-matmul work spent on the requests the block served, divided by all of that work. The ticket\n"
        "work is estimated from each block's difficulty (expected tickets x r^3).\n",
        {
            {"nblocks", RPCArg::Type::NUM, /* default */ "144", "Number of blocks, counting back from blockhash"},
            {"blockhash", RPCArg::Type::STR_HEX, /* default */ "chain tip", "Last block of the window"},
            {"verbose", RPCArg::Type::BOOL, /* default */ "false", "Include one entry per block"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "blocks", "v2 blocks in the window"},
                {RPCResult::Type::NUM, "requests", "Embedding requests served"},
                {RPCResult::Type::NUM, "served_tokens", "Model input tokens of the served requests (EOS included)"},
                {RPCResult::Type::NUM, "empty_blocks", "Blocks that served no request"},
                {RPCResult::Type::NUM, "macs_per_token", "Weight-matmul multiply-accumulates per token of the protocol model"},
                {RPCResult::Type::NUM, "useful_macs", "served_tokens x macs_per_token"},
                {RPCResult::Type::NUM, "pass_macs", "Clean forward passes (empty blocks run the EOS-only input)"},
                {RPCResult::Type::NUM, "ticket_macs", "Expected ticket search work"},
                {RPCResult::Type::NUM, "useful_share", "useful_macs / (pass_macs + ticket_macs)"},
                {RPCResult::Type::ARR, "per_block", /* optional */ true, "With verbose",
                    {{RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::NUM, "height", "Height"},
                            {RPCResult::Type::STR_HEX, "hash", "Block hash"},
                            {RPCResult::Type::NUM, "requests", "Requests served"},
                            {RPCResult::Type::NUM, "served_tokens", "Tokens served"},
                            {RPCResult::Type::NUM, "useful_share", "Useful share of this block"},
                        }}}},
            }},
        RPCExamples{HelpExampleCli("getusefulshare", "") + HelpExampleCli("getusefulshare", "2016")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const intmodel::IntModel* model = embed::GetProtocolModel();
    if (!model) throw JSONRPCError(RPC_MISC_ERROR, "No protocol model loaded (ApertureMatMul v2 inactive)");
    const int nblocks = request.params[0].isNull() ? 144 : request.params[0].get_int();
    if (nblocks < 1) throw JSONRPCError(RPC_INVALID_PARAMETER, "nblocks must be positive");
    const bool verbose = !request.params[2].isNull() && request.params[2].get_bool();

    double macs_per_token = 0;
    for (const matmulpow_v2::Op& op : matmulpow_v2::GetOps()) macs_per_token += double(op.d_in) * op.d_out;
    const double r = matmulpow_v2::GetRank();
    const double ticket_cost = r * r * r;

    uint64_t blocks = 0, requests = 0, served = 0, empty = 0;
    double useful = 0, pass = 0, tickets = 0;
    UniValue per_block(UniValue::VARR);
    LOCK(cs_main);
    const CBlockIndex* pindex = ::ChainActive().Tip();
    if (!request.params[1].isNull()) {
        pindex = LookupBlockIndex(ParseHashV(request.params[1], "blockhash"));
        if (!pindex) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
    }
    for (int k = 0; pindex && k < nblocks; ++k, pindex = pindex->pprev) {
        if (!(pindex->nVersion & CBlockHeader::VERSION_POWV2)) break;
        CBlock block;
        if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) throw JSONRPCError(RPC_MISC_ERROR, "Block not available (pruned data)");
        uint64_t block_tokens = 0;
        const std::vector<embed::Request> reqs = embed::CollectRequests(block);
        for (const embed::Request& req : reqs) block_tokens += req.ids.size() + 1;
        const double block_useful = double(block_tokens) * macs_per_token;
        const double block_pass = double(std::max<uint64_t>(block_tokens, 1)) * macs_per_token;
        const double block_tickets = GetBlockProof(*pindex).getdouble() * ticket_cost;
        ++blocks;
        requests += reqs.size();
        served += block_tokens;
        empty += reqs.empty();
        useful += block_useful;
        pass += block_pass;
        tickets += block_tickets;
        if (verbose) {
            UniValue b(UniValue::VOBJ);
            b.pushKV("height", pindex->nHeight);
            b.pushKV("hash", pindex->GetBlockHash().GetHex());
            b.pushKV("requests", (uint64_t)reqs.size());
            b.pushKV("served_tokens", block_tokens);
            b.pushKV("useful_share", block_useful / (block_pass + block_tickets));
            per_block.push_back(b);
        }
    }
    UniValue out(UniValue::VOBJ);
    out.pushKV("blocks", blocks);
    out.pushKV("requests", requests);
    out.pushKV("served_tokens", served);
    out.pushKV("empty_blocks", empty);
    out.pushKV("macs_per_token", macs_per_token);
    out.pushKV("useful_macs", useful);
    out.pushKV("pass_macs", pass);
    out.pushKV("ticket_macs", tickets);
    out.pushKV("useful_share", blocks ? useful / (pass + tickets) : 0.0);
    if (verbose) out.pushKV("per_block", per_block);
    return out;
},
    };
}

void RegisterEmbedRPCCommands(CRPCTable& t)
{
    // clang-format off
    static const CRPCCommand commands[] =
    { //  category              name                        actor (function)            argNames
      //  --------------------- ------------------------    -----------------------     ----------
        { "embedding",          "embed",                    &embed_rpc,                 {"input"} },
        { "embedding",          "createembeddingrequest",   &createembeddingrequest,    {"input"} },
        { "embedding",          "getblockembeddings",       &getblockembeddings,        {"blockhash"} },
        { "embedding",          "searchembeddings",         &searchembeddings,          {"input", "blocks", "count"} },
        { "embedding",          "checkblockembeddings",     &checkblockembeddings,      {"blockhash"} },
        { "embedding",          "createfraudclaim",         &createfraudclaim,          {"blockhash", "index", "address", "fee"} },
        { "embedding",          "estimaterequestfee",       &estimaterequestfee,        {"input"} },
        { "embedding",          "getusefulshare",           &getusefulshare,            {"nblocks", "blockhash", "verbose"} },
    };
    // clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
