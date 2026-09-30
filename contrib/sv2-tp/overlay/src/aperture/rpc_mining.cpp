// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <aperture/rpc_mining.h>

#include <consensus/merkle.h>
#include <logging.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <sv2/messages.h>
#include <util/strencodings.h>

#include <chrono>
#include <thread>

namespace aperture {
namespace {

using namespace std::chrono_literals;

constexpr auto TIP_POLL_INTERVAL{250ms};
constexpr auto FEE_POLL_INTERVAL{2s};

UniValue GbtRequest()
{
    UniValue rules(UniValue::VARR);
    rules.push_back("segwit");
    UniValue request(UniValue::VOBJ);
    request.pushKV("rules", rules);
    UniValue params(UniValue::VARR);
    params.push_back(request);
    return params;
}

std::string SerializeBlockHex(const CBlock& block)
{
    DataStream ss;
    ss << TX_WITH_WITNESS(block);
    return HexStr(ss);
}

CTransactionRef DecodeTx(const std::string& hex)
{
    DataStream ss{ParseHex(hex)};
    CMutableTransaction mtx;
    ss >> TX_WITH_WITNESS(mtx);
    return MakeTransactionRef(std::move(mtx));
}

uint256 HashFromRpc(const UniValue& value)
{
    auto hash{uint256::FromHex(value.get_str())};
    if (!hash) throw RpcTransportError("invalid hash in RPC reply");
    return *hash;
}

/** Poll until the best block differs from current_tip, the timeout expires or a wait is interrupted. */
std::optional<uint256> WaitForTipChange(RpcMiningState& state, const uint256& current_tip, MillisecondsDouble timeout)
{
    const uint64_t generation{state.interrupt_generation.load()};
    const auto deadline{timeout == MillisecondsDouble::max()
                            ? std::chrono::steady_clock::time_point::max()
                            : std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout)};
    while (true) {
        const uint256 tip{HashFromRpc(state.rpc.Call("getbestblockhash"))};
        if (tip != current_tip) return tip;
        if (state.interrupt_generation.load() != generation) return std::nullopt;
        if (std::chrono::steady_clock::now() + TIP_POLL_INTERVAL > deadline) return std::nullopt;
        std::this_thread::sleep_for(TIP_POLL_INTERVAL);
    }
}

class RpcBlockTemplate : public interfaces::BlockTemplate
{
public:
    RpcBlockTemplate(std::shared_ptr<RpcMiningState> state, const UniValue& gbt) : m_state(std::move(state))
    {
        const int height{find_value(gbt, "height").get_int()};
        const CAmount coinbase_value{find_value(gbt, "coinbasevalue").get_int64()};

        m_block.nVersion = find_value(gbt, "version").get_int();
        m_block.hashPrevBlock = HashFromRpc(find_value(gbt, "previousblockhash"));
        m_block.nTime = static_cast<uint32_t>(find_value(gbt, "curtime").get_int64());
        m_block.nBits = static_cast<uint32_t>(std::stoul(find_value(gbt, "bits").get_str(), nullptr, 16));
        m_block.nNonce = 0;

        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << height; // BIP34; miners append extranonce
        coinbase.vin[0].nSequence = CTxIn::SEQUENCE_FINAL;

        // Output 0: placeholder for the miner's reward (replaced by SV2 clients).
        CAmount reward{coinbase_value};
        std::vector<CTxOut> required;
        const UniValue& devfund = find_value(gbt, "devfund");
        if (devfund.isObject()) {
            const CAmount amount{find_value(devfund, "amount").get_int64()};
            const auto script{ParseHex(find_value(devfund, "script").get_str())};
            required.emplace_back(amount, CScript(script.begin(), script.end()));
            reward -= amount;
        }
        // ApertureMatMul v2: the coinbase results serving the block's
        // embedding requests are mandatory outputs, like the development fund.
        const UniValue& powv2 = find_value(gbt, "powv2");
        if (powv2.isObject()) {
            m_powv2 = true;
            const auto root{ParseHex(find_value(powv2, "batch_root").get_str())};
            const auto model{ParseHex(find_value(powv2, "model_id").get_str())};
            if (root.size() != 32 || model.size() != 32) throw RpcTransportError("bad powv2 object");
            m_batch_root.assign(root.begin(), root.end());
            m_model_id.assign(model.begin(), model.end());
            m_rank = static_cast<uint8_t>(find_value(powv2, "rank").get_int());
            for (const UniValue& req : find_value(powv2, "requests").getValues()) {
                std::vector<uint32_t> ids;
                for (const UniValue& id : req.getValues()) ids.push_back(static_cast<uint32_t>(id.get_int64()));
                m_requests.push_back(std::move(ids));
            }
            for (const UniValue& script_hex : find_value(powv2, "results").getValues()) {
                const auto script{ParseHex(script_hex.get_str())};
                required.emplace_back(0, CScript(script.begin(), script.end()));
            }
        }
        const UniValue& commitment = find_value(gbt, "default_witness_commitment");
        if (commitment.isStr()) {
            const auto script{ParseHex(commitment.get_str())};
            required.emplace_back(0, CScript(script.begin(), script.end()));
            // BIP141 witness reserved value (all zero, matching the node's commitment).
            coinbase.vin[0].scriptWitness.stack.emplace_back(32, 0);
        }
        coinbase.vout.emplace_back(reward, CScript() << OP_TRUE);
        for (auto& out : required) coinbase.vout.push_back(std::move(out));
        m_block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));

        for (const UniValue& tx : find_value(gbt, "transactions").getValues()) {
            m_block.vtx.push_back(DecodeTx(find_value(tx, "data").get_str()));
            m_fees.push_back(find_value(tx, "fee").get_int64());
            m_sigops.push_back(find_value(tx, "sigops").get_int64());
        }
        m_block.hashMerkleRoot = BlockMerkleRoot(m_block);
    }

    CBlockHeader getBlockHeader() override { return m_block.GetBlockHeader(); }
    CBlock getBlock() override { return m_block; }
    std::vector<CAmount> getTxFees() override { return m_fees; }
    std::vector<int64_t> getTxSigops() override { return m_sigops; }
    node::CoinbaseTx getCoinbaseTx() override { return ExtractCoinbaseTx(m_block.vtx[0]); }
    std::vector<uint256> getCoinbaseMerklePath() override { return TransactionMerklePath(m_block, 0); }

    bool submitSolution(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase,
                        std::string& reason, std::string& debug) override
    {
        CBlock block{m_block};
        block.nVersion = static_cast<int32_t>(version);
        block.nTime = timestamp;
        block.nNonce = nonce;
        block.vtx[0] = std::move(coinbase);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        UniValue params(UniValue::VARR);
        params.push_back(SerializeBlockHex(block));
        try {
            const UniValue result{m_state->rpc.Call("submitblock", params)};
            if (result.isNull()) return true;
            reason = result.isStr() ? result.get_str() : result.write();
        } catch (const std::exception& e) {
            reason = "rpc-error";
            debug = e.what();
        }
        return false;
    }

    bool submitSolutionOld7(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase) override
    {
        std::string reason, debug;
        return submitSolution(version, timestamp, nonce, std::move(coinbase), reason, debug);
    }

    std::unique_ptr<interfaces::BlockTemplate> waitNext(node::BlockWaitOptions options) override
    {
        const uint64_t generation{m_state->interrupt_generation.load()};
        const auto start{std::chrono::steady_clock::now()};
        const auto deadline{options.timeout == MillisecondsDouble::max()
                                ? std::chrono::steady_clock::time_point::max()
                                : start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(options.timeout)};
        CAmount my_fees{0};
        for (CAmount fee : m_fees) my_fees += fee;
        auto next_fee_check{start + FEE_POLL_INTERVAL};

        while (true) {
            if (m_state->interrupt_generation.load() != generation) return nullptr;
            const uint256 tip{HashFromRpc(m_state->rpc.Call("getbestblockhash"))};
            if (tip != m_block.hashPrevBlock) return MakeRpcBlockTemplate(m_state);

            const auto now{std::chrono::steady_clock::now()};
            if (now >= next_fee_check) {
                next_fee_check = now + FEE_POLL_INTERVAL;
                auto candidate{MakeRpcBlockTemplate(m_state)};
                CAmount fees{0};
                for (CAmount fee : candidate->getTxFees()) fees += fee;
                if (candidate->getBlockHeader().hashPrevBlock != m_block.hashPrevBlock ||
                    fees - my_fees >= options.fee_threshold) {
                    return candidate;
                }
            }
            if (now + TIP_POLL_INTERVAL > deadline) return nullptr;
            std::this_thread::sleep_for(TIP_POLL_INTERVAL);
        }
    }

    void interruptWait() override { ++m_state->interrupt_generation; }

    /**
     * UsefulWorkTemplate payload (doc/stratum-v2.md): batch_root[32] model_id[32]
     * rank:u8 request_count:u16 { token_count:u16 token_id:u24... }
     */
    std::vector<uint8_t> apertureUsefulWork() override
    {
        if (!m_powv2) return {};
        std::vector<uint8_t> out(m_batch_root);
        out.insert(out.end(), m_model_id.begin(), m_model_id.end());
        out.push_back(m_rank);
        const auto u16 = [&](uint16_t v) { out.push_back(v & 0xff); out.push_back(v >> 8); };
        u16(static_cast<uint16_t>(m_requests.size()));
        for (const auto& ids : m_requests) {
            u16(static_cast<uint16_t>(ids.size()));
            for (uint32_t id : ids) {
                out.push_back(id & 0xff);
                out.push_back((id >> 8) & 0xff);
                out.push_back((id >> 16) & 0xff);
            }
        }
        return out;
    }

    /**
     * SubmitUsefulWorkSolution payload: version:u32 timestamp:u32 nonce:u32
     * op:u16 tile_i:u16 tile_j:u16 span_s:u16 panel_len:u16 panel[panel_len]
     * coinbase_len:u32 coinbase[coinbase_len] (with witness).
     */
    bool apertureSubmitUsefulWork(const std::vector<uint8_t>& payload, std::string& reason) override
    {
        if (!m_powv2) {
            reason = "template-not-v2";
            return false;
        }
        CMutableTransaction coinbase;
        uint32_t version, timestamp, nonce;
        uint16_t ticket[4], panel_len;
        std::vector<uint8_t> panel;
        try {
            SpanReader r{payload};
            r >> version >> timestamp >> nonce >> ticket[0] >> ticket[1] >> ticket[2] >> ticket[3] >> panel_len;
            panel.resize(panel_len);
            r.read(MakeWritableByteSpan(panel));
            uint32_t cb_len;
            r >> cb_len;
            if (cb_len != r.size()) throw std::ios_base::failure("coinbase length");
            r >> TX_WITH_WITNESS(coinbase);
        } catch (const std::exception& e) {
            reason = std::string{"bad-useful-work-solution: "} + e.what();
            return false;
        }
        CBlock block{m_block};
        block.nVersion = static_cast<int32_t>(version);
        block.nTime = timestamp;
        block.nNonce = nonce;
        block.vtx[0] = MakeTransactionRef(std::move(coinbase));
        block.hashMerkleRoot = BlockMerkleRoot(block);

        // ApertureCoin v2 header extension after the 80 v1 header bytes:
        // batch_root, op, tile_i, tile_j, span_s, compact size, panel.
        std::vector<unsigned char> raw{ParseHex(SerializeBlockHex(block))};
        std::vector<unsigned char> ext(m_batch_root);
        for (uint16_t v : ticket) {
            ext.push_back(v & 0xff);
            ext.push_back(v >> 8);
        }
        if (panel.size() < 0xfd) {
            ext.push_back(static_cast<unsigned char>(panel.size()));
        } else {
            ext.push_back(0xfd);
            ext.push_back(panel.size() & 0xff);
            ext.push_back((panel.size() >> 8) & 0xff);
        }
        ext.insert(ext.end(), panel.begin(), panel.end());
        raw.insert(raw.begin() + 80, ext.begin(), ext.end());

        UniValue params(UniValue::VARR);
        params.push_back(HexStr(raw));
        try {
            const UniValue result{m_state->rpc.Call("submitblock", params)};
            if (result.isNull()) return true;
            reason = result.isStr() ? result.get_str() : result.write();
        } catch (const std::exception& e) {
            reason = std::string{"rpc-error: "} + e.what();
        }
        return false;
    }

private:
    bool m_powv2{false};
    std::vector<uint8_t> m_batch_root, m_model_id;
    uint8_t m_rank{0};
    std::vector<std::vector<uint32_t>> m_requests;
    std::shared_ptr<RpcMiningState> m_state;
    CBlock m_block;
    std::vector<CAmount> m_fees;
    std::vector<int64_t> m_sigops;
};

} // namespace

std::unique_ptr<interfaces::BlockTemplate> MakeRpcBlockTemplate(std::shared_ptr<RpcMiningState> state)
{
    const UniValue gbt{state->rpc.Call("getblocktemplate", GbtRequest())};
    return std::make_unique<RpcBlockTemplate>(std::move(state), gbt);
}

bool RpcMining::isTestChain()
{
    return find_value(m_state->rpc.Call("getblockchaininfo"), "chain").get_str() != "main";
}

bool RpcMining::isInitialBlockDownload()
{
    return find_value(m_state->rpc.Call("getblockchaininfo"), "initialblockdownload").get_bool();
}

std::optional<interfaces::BlockRef> RpcMining::getTip()
{
    const uint256 hash{HashFromRpc(m_state->rpc.Call("getbestblockhash"))};
    UniValue params(UniValue::VARR);
    params.push_back(hash.GetHex());
    const UniValue header{m_state->rpc.Call("getblockheader", params)};
    return interfaces::BlockRef{hash, find_value(header, "height").get_int()};
}

std::optional<interfaces::BlockRef> RpcMining::waitTipChanged(uint256 current_tip, MillisecondsDouble timeout)
{
    if (!WaitForTipChange(*m_state, current_tip, timeout)) return getTip();
    return getTip();
}

std::unique_ptr<interfaces::BlockTemplate> RpcMining::createNewBlock(const node::BlockCreateOptions&, bool)
{
    return MakeRpcBlockTemplate(m_state);
}

void RpcMining::interrupt() { ++m_state->interrupt_generation; }

bool RpcMining::checkBlock(const CBlock& block, const node::BlockCheckOptions&, std::string& reason, std::string& debug)
{
    UniValue request(UniValue::VOBJ);
    request.pushKV("mode", "proposal");
    request.pushKV("data", SerializeBlockHex(block));
    UniValue rules(UniValue::VARR);
    rules.push_back("segwit");
    request.pushKV("rules", rules);
    UniValue params(UniValue::VARR);
    params.push_back(request);
    try {
        const UniValue result{m_state->rpc.Call("getblocktemplate", params)};
        if (result.isNull()) return true;
        reason = result.isStr() ? result.get_str() : result.write();
    } catch (const std::exception& e) {
        reason = "rpc-error";
        debug = e.what();
    }
    return false;
}

bool RpcMining::submitBlock(const CBlock& block, std::string& reason, std::string& debug)
{
    UniValue params(UniValue::VARR);
    params.push_back(SerializeBlockHex(block));
    try {
        const UniValue result{m_state->rpc.Call("submitblock", params)};
        if (result.isNull()) return true;
        reason = result.isStr() ? result.get_str() : result.write();
    } catch (const std::exception& e) {
        reason = "rpc-error";
        debug = e.what();
    }
    return false;
}

std::vector<CTransactionRef> RpcMining::getTransactionsByTxID(const std::vector<Txid>& txids)
{
    std::vector<CTransactionRef> result;
    result.reserve(txids.size());
    for (const Txid& txid : txids) {
        UniValue params(UniValue::VARR);
        params.push_back(txid.GetHex());
        try {
            result.push_back(DecodeTx(m_state->rpc.Call("getrawtransaction", params).get_str()));
        } catch (const RpcError&) {
            result.push_back(nullptr);
        }
    }
    return result;
}

} // namespace aperture
