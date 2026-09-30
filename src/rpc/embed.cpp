// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <model/embed.h>
#include <primitives/block.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <util/strencodings.h>
#include <validation.h>

#include <cmath>

static std::vector<uint32_t> ParseEmbedInput(const UniValue& v)
{
    const intmodel::IntModel* model = embed::GetProtocolModel();
    if (!model) throw JSONRPCError(RPC_MISC_ERROR, "No protocol model loaded (ApertureMatMul v2 inactive)");
    std::vector<uint32_t> ids;
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

static std::vector<int8_t> EmbedIds(const std::vector<uint32_t>& ids)
{
    const intmodel::IntModel* model = embed::GetProtocolModel();
    return embed::Embed(*model, embed::ModelInput(*model, ids));
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
            }},
        RPCExamples{HelpExampleCli("embed", "\"hello world\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::vector<uint32_t> ids = ParseEmbedInput(request.params[0]);
    const std::vector<int8_t> e = EmbedIds(ids);
    UniValue out(UniValue::VOBJ);
    out.pushKV("model_id", embed::GetProtocolModel()->Apm().ModelIdHex());
    out.pushKV("tokens", (uint64_t)ids.size() + 1);
    out.pushKV("embedding", I8Hex(e));
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
    };
    // clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
