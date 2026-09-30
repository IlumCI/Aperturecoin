// Copyright (c) 2017-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/tx_verify.h>

#include <consensus/fraudclaim.h>

#include <chainparams.h>
#include <consensus/consensus.h>
#include <primitives/token.h>
#include <set>
#include <map>
#include <limits>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <consensus/validation.h>

// TODO remove the following dependencies
#include <chain.h>
#include <coins.h>
#include <util/moneystr.h>

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    // MWEB: Check kernel lock heights
    if (tx.mweb_tx.GetLockHeight() > nBlockHeight)
        return false;

    if (tx.nLockTime == 0)
        return true;
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    for (const auto& txin : tx.vin) {
        if (!(txin.nSequence == CTxIn::SEQUENCE_FINAL))
            return false;
    }
    return true;
}

std::pair<int, int64_t> CalculateSequenceLocks(const CTransaction &tx, int flags, std::vector<int>& prevHeights, const CBlockIndex& block)
{
    assert(prevHeights.size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // tx.nVersion is signed integer so requires cast to unsigned otherwise
    // we would be doing a signed comparison and half the range of nVersion
    // wouldn't support BIP 68.
    bool fEnforceBIP68 = static_cast<uint32_t>(tx.nVersion) >= 2
                      && flags & LOCKTIME_VERIFY_SEQUENCE;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68) {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
        const CTxIn& txin = tx.vin[txinIndex];

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            // The height of this input is not relevant for sequence locks
            prevHeights[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = prevHeights[txinIndex];

        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
            int64_t nCoinTime = block.GetAncestor(std::max(nCoinHeight-1, 0))->GetMedianTimePast();
            // NOTE: Subtract 1 to maintain nLockTime semantics
            // BIP 68 relative lock times have the semantics of calculating
            // the first block or time at which the transaction would be
            // valid. When calculating the effective block time or height
            // for the entire transaction, we switch to using the
            // semantics of nLockTime which is the last invalid block
            // time or height.  Thus we subtract 1 from the calculated
            // time or height.

            // Time-based relative lock-times are measured from the
            // smallest allowed timestamp of the block containing the
            // txout being spent, which is the median time past of the
            // block prior.
            nMinTime = std::max(nMinTime, nCoinTime + (int64_t)((txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1);
        } else {
            nMinHeight = std::max(nMinHeight, nCoinHeight + (int)(txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}

bool EvaluateSequenceLocks(const CBlockIndex& block, std::pair<int, int64_t> lockPair)
{
    assert(block.pprev);
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const CTransaction &tx, int flags, std::vector<int>& prevHeights, const CBlockIndex& block)
{
    return EvaluateSequenceLocks(block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    for (const auto& txin : tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (const auto& txout : tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }

    // MWEB: Include pegout scripts
    for (const PegOutCoin& pegout : tx.mweb_tx.GetPegOuts()) {
        nSigOps += pegout.GetScriptPubKey().GetSigOpCount(false);
    }

    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const CCoinsViewCache& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const CTxOut &prevout = coin.out;
        const CScript locking_bytecode{token::GetLockingBytecode(prevout.scriptPubKey)};
        if (locking_bytecode.IsPayToScriptHash())
            nSigOps += locking_bytecode.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

int64_t GetTransactionSigOpCost(const CTransaction& tx, const CCoinsViewCache& inputs, int flags)
{
    int64_t nSigOps = GetLegacySigOpCount(tx) * WITNESS_SCALE_FACTOR;

    if (tx.IsCoinBase())
        return nSigOps;

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs) * WITNESS_SCALE_FACTOR;
    }

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const CTxOut &prevout = coin.out;
        nSigOps += CountWitnessSigOps(tx.vin[i].scriptSig, token::GetLockingBytecode(prevout.scriptPubKey), &tx.vin[i].scriptWitness, flags);
    }
    return nSigOps;
}

namespace {

/**
 * Native token conservation (doc/tokens.md, CashTokens CHIP-2022-02 rules):
 * - a new category can only be created by an input spending an outpoint with
 *   index 0; its ID is that outpoint's txid (genesis)
 * - fungible amounts per category cannot exceed the inputs' amounts, except
 *   for categories created in this transaction
 * - NFTs must be justified by inputs: a minting NFT (or genesis) allows any
 *   NFT of its category; a mutable NFT allows one NFT with any commitment and
 *   capability none/mutable; an immutable NFT passes through unchanged
 * Output token prefixes were checked by CheckTransaction.
 */
bool CheckTokenConservation(const CTransaction& tx, TxValidationState& state, const CCoinsViewCache& inputs)
{
    bool any_tokens{false};
    for (const CTxOut& out : tx.vout) any_tokens |= token::HasTokenPrefix(out.scriptPubKey);
    for (const CTxIn& in : tx.vin) any_tokens |= token::HasTokenPrefix(inputs.AccessCoin(in.prevout).out.scriptPubKey);
    if (!any_tokens) return true;

    std::set<uint256> genesis;
    std::set<uint256> categories_in;
    std::set<uint256> minting_in;
    std::map<uint256, int64_t> ft_in;
    std::map<uint256, std::multiset<std::vector<unsigned char>>> immutable_in;
    std::map<uint256, int64_t> mutable_in;

    for (const CTxIn& in : tx.vin) {
        if (in.prevout.n == 0) genesis.insert(in.prevout.hash);
        const CScript& spk{inputs.AccessCoin(in.prevout).out.scriptPubKey};
        token::TokenData td;
        const token::ParseResult parsed{token::Parse(spk, td)};
        if (parsed == token::ParseResult::NO_TOKEN) continue;
        if (parsed != token::ParseResult::OK) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-input-prefix");
        }
        categories_in.insert(td.category);
        if (td.amount > 0) {
            int64_t& sum{ft_in[td.category]};
            if (td.amount > std::numeric_limits<int64_t>::max() - sum) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-amount-overflow");
            }
            sum += td.amount;
        }
        if (td.nft) {
            switch (td.nft->capability) {
            case token::Capability::NONE: immutable_in[td.category].insert(td.nft->commitment); break;
            case token::Capability::MUTABLE: ++mutable_in[td.category]; break;
            case token::Capability::MINTING: minting_in.insert(td.category); break;
            }
        }
    }

    std::map<uint256, int64_t> ft_out;
    std::vector<std::pair<uint256, token::NFT>> nft_out;
    for (const CTxOut& out : tx.vout) {
        token::TokenData td;
        if (token::Parse(out.scriptPubKey, td) != token::ParseResult::OK) continue;
        if (!categories_in.count(td.category) && !genesis.count(td.category)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-category",
                                 strprintf("category %s has no input and no genesis", td.category.GetHex()));
        }
        if (td.amount > 0) {
            int64_t& sum{ft_out[td.category]};
            if (td.amount > std::numeric_limits<int64_t>::max() - sum) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-amount-overflow");
            }
            sum += td.amount;
        }
        if (td.nft) nft_out.emplace_back(td.category, *td.nft);
    }

    for (const auto& [category, amount] : ft_out) {
        if (genesis.count(category)) continue;
        if (amount > ft_in[category]) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-amount-inflation",
                                 strprintf("category %s: %d out > %d in", category.GetHex(), amount, ft_in[category]));
        }
    }

    // Pass 1: immutable outputs that exactly match an immutable input. Pass 2:
    // remaining outputs consume mutable inputs. Using exact matches first is
    // optimal, since mutable inputs can justify any non-minting output.
    std::vector<const std::pair<uint256, token::NFT>*> unmatched;
    for (const auto& entry : nft_out) {
        const auto& [category, nft] = entry;
        if (genesis.count(category) || minting_in.count(category)) continue;
        if (nft.capability == token::Capability::MINTING) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-nft-minting");
        }
        if (nft.capability == token::Capability::NONE) {
            auto& available{immutable_in[category]};
            const auto it{available.find(nft.commitment)};
            if (it != available.end()) {
                available.erase(it);
                continue;
            }
        }
        unmatched.push_back(&entry);
    }
    for (const auto* entry : unmatched) {
        int64_t& slots{mutable_in[entry->first]};
        if (slots <= 0) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-nft-ex-nihilo",
                                 strprintf("category %s: NFT not justified by inputs", entry->first.GetHex()));
        }
        --slots;
    }
    return true;
}

} // namespace

bool Consensus::CheckTxInputs(const CTransaction& tx, TxValidationState& state, const CCoinsViewCache& inputs, int nSpendHeight, CAmount& txfee)
{
    const auto& consensus_params = ::Params().GetConsensus();

    // are the actual inputs available?
    if (!inputs.HaveInputs(tx)) {
        return state.Invalid(TxValidationResult::TX_MISSING_INPUTS, "bad-txns-inputs-missingorspent",
                         strprintf("%s: inputs missing/spent", __func__));
    }

    CAmount nValueIn = 0;
    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        const COutPoint &prevout = tx.vin[i].prevout;
        const Coin& coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        // If prev is coinbase, check that it's matured
        // Fraud claims may take an immature coinbase: that is the forfeit
        // (consensus/fraudclaim.h, CheckFraudClaim in validation.cpp).
        if (coin.IsCoinBase() && nSpendHeight - coin.nHeight < COINBASE_MATURITY && !IsFraudClaimTx(tx)) {
            return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND, "bad-txns-premature-spend-of-coinbase",
                strprintf("tried to spend coinbase at depth %d", nSpendHeight - coin.nHeight));
        }

        // If coin is a pegout, check that it's matured
        if (coin.IsPegout() && nSpendHeight - coin.nHeight < PEGOUT_MATURITY) {
            return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND, "bad-txns-premature-spend-of-pegout",
                strprintf("tried to spend pegout output at depth %d", nSpendHeight - coin.nHeight));
        }

        // Check for negative or overflow input values
        nValueIn += coin.out.nValue;
        if (!MoneyRange(coin.out.nValue) || !MoneyRange(nValueIn)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-inputvalues-outofrange");
        }
    }

    const CAmount value_out = tx.GetValueOut();
    if (nValueIn < value_out) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-in-belowout",
            strprintf("value in (%s) < value out (%s)", FormatMoney(nValueIn), FormatMoney(value_out)));
    }

    if (!CheckTokenConservation(tx, state, inputs)) {
        return false; // state filled in by CheckTokenConservation
    }

    // Tally transaction fees
    CAmount txfee_aux = nValueIn - value_out;
    if (!MoneyRange(txfee_aux)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-fee-outofrange");
    }

    // MWEB
    if (tx.HasMWEBTx()) {
        for (const Input& input : tx.mweb_tx.m_transaction->GetInputs()) {
            Output utxo;
            if (!inputs.GetMWEBCoin(input.GetOutputID(), utxo)) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-inputs-missing",
                    strprintf("%s: MWEB inputs missing", __func__));
            }

            for (const uint256& frozen_output_id : consensus_params.frozen_mweb_output_ids) {
                if (uint256(input.GetOutputID().vec()) == frozen_output_id) {
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-frozen-mweb-output",
                        strprintf("%s: spends frozen MWEB output %s", __func__, input.GetOutputID().ToHex()));
                }
            }

            if (utxo.GetReceiverPubKey() != input.GetOutputPubKey() || utxo.GetCommitment() != input.GetCommitment()) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-input-mismatch",
                                     strprintf("%s: MWEB input doesn't match UTXO", __func__));
            }
        }

        const auto mweb_fee = tx.mweb_tx.GetFee();
        if (!mweb_fee) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-mwebfee-outofrange");
        }

        txfee_aux += *mweb_fee;
        if (!MoneyRange(*mweb_fee) || !MoneyRange(txfee_aux)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-mwebfee-outofrange");
        }
    }

    txfee = txfee_aux;
    return true;
}
