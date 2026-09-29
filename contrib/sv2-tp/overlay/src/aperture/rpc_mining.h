// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef APERTURE_RPC_MINING_H
#define APERTURE_RPC_MINING_H

#include <aperture/rpc_client.h>
#include <interfaces/mining.h>

#include <atomic>
#include <memory>

namespace aperture {

/** State shared by the Mining backend and all templates it created. */
struct RpcMiningState {
    explicit RpcMiningState(RpcClient client) : rpc(std::move(client)) {}
    RpcClient rpc;
    //! Incremented by interrupt()/interruptWait(); waits observe a change and return.
    std::atomic<uint64_t> interrupt_generation{0};
};

/**
 * interfaces::Mining implemented on top of apertured's JSON-RPC interface
 * (getblocktemplate / submitblock). ApertureCoin Core 0.21 has no Mining IPC
 * interface, so sv2-tp talks to it over RPC instead.
 *
 * Coinbase layout of every template: output 0 is the placeholder carrying the
 * reward available to the miner; every further output (development fund,
 * witness commitment) is mandatory and forwarded to SV2 clients as
 * coinbase_tx_outputs.
 */
class RpcMining : public interfaces::Mining
{
public:
    explicit RpcMining(std::shared_ptr<RpcMiningState> state) : m_state(std::move(state)) {}

    bool isTestChain() override;
    bool isInitialBlockDownload() override;
    std::optional<interfaces::BlockRef> getTip() override;
    std::optional<interfaces::BlockRef> waitTipChanged(uint256 current_tip, MillisecondsDouble timeout) override;
    std::unique_ptr<interfaces::BlockTemplate> createNewBlock(const node::BlockCreateOptions& options, bool cooldown) override;
    void interrupt() override;
    bool checkBlock(const CBlock& block, const node::BlockCheckOptions& options, std::string& reason, std::string& debug) override;
    bool submitBlock(const CBlock& block, std::string& reason, std::string& debug) override;
    std::vector<CTransactionRef> getTransactionsByTxID(const std::vector<Txid>& txids) override;

private:
    std::shared_ptr<RpcMiningState> m_state;
};

/** Build a template from getblocktemplate (exposed for tests). */
std::unique_ptr<interfaces::BlockTemplate> MakeRpcBlockTemplate(std::shared_ptr<RpcMiningState> state);

} // namespace aperture

#endif // APERTURE_RPC_MINING_H
