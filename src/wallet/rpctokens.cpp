// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Wallet RPCs for native tokens (doc/tokens.md).

#include <core_io.h>
#include <model/embed.h>
#include <key_io.h>
#include <policy/policy.h>
#include <primitives/token.h>
#include <rpc/util.h>
#include <script/standard.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/rpcwallet.h>
#include <wallet/wallet.h>

#include <map>
#include <optional>

namespace {

const std::string HELP_REQUIRING_PASSPHRASE{"\nRequires wallet passphrase to be set with walletpassphrase call if wallet is encrypted.\n"};

struct TokenCoin {
    COutPoint outpoint;
    CTxOut txout;
    token::TokenData token_data;
    int depth;
};

/** Spendable, unspent wallet outputs carrying native tokens. */
std::vector<TokenCoin> ListTokenCoins(const CWallet& wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    std::vector<TokenCoin> coins;
    std::set<uint256> trusted_parents;
    for (const auto& [txid, wtx] : wallet.mapWallet) {
        const int depth{wtx.GetDepthInMainChain()};
        if (depth < 0) continue;
        if (depth == 0 && !wallet.IsTrusted(wtx, trusted_parents)) continue;
        for (uint32_t i = 0; i < wtx.tx->vout.size(); ++i) {
            const CTxOut& txout{wtx.tx->vout[i]};
            if (!token::HasTokenPrefix(txout.scriptPubKey)) continue;
            const COutPoint outpoint{txid, i};
            if (wallet.IsSpent(outpoint.hash, outpoint.n) || wallet.IsLockedCoin(outpoint.hash, outpoint.n)) continue;
            if (!(wallet.IsMine(txout) & ISMINE_SPENDABLE)) continue;
            token::TokenData td;
            if (token::Parse(txout.scriptPubKey, td) != token::ParseResult::OK) continue;
            coins.push_back({outpoint, txout, td, depth});
        }
    }
    return coins;
}

CScript NewChangeScript(CWallet& wallet)
{
    CTxDestination dest;
    std::string error;
    if (!wallet.GetNewChangeDestination(wallet.m_default_change_type ? *wallet.m_default_change_type : wallet.m_default_address_type, dest, error)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, error);
    }
    return GetScriptForDestination(dest);
}

/** A token-carrying recipient with the minimum non-dust SCIENCE value. */
CRecipient TokenRecipient(CWallet& wallet, const token::TokenData& td, const CScript& locking_bytecode)
{
    const CScript script{token::Encode(td, locking_bytecode)};
    const CAmount value{GetDustThreshold(CTxOut(0, script), wallet.chain().relayDustFee())};
    return CRecipient{script, std::max<CAmount>(value, 1), false};
}

UniValue CreateAndCommit(CWallet& wallet, std::vector<CRecipient>& recipients, const CCoinControl& coin_control) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    EnsureWalletIsUnlocked(&wallet);
    CAmount fee{0};
    int change_pos{-1};
    bilingual_str error;
    CTransactionRef tx;
    FeeCalculation fee_calc;
    if (!wallet.CreateTransaction(recipients, tx, fee, change_pos, error, coin_control, fee_calc, true)) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, error.original);
    }
    wallet.CommitTransaction(tx, {}, {});
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("fee", ValueFromAmount(fee));
    return result;
}

CScript LockingBytecodeForAddress(const UniValue& address)
{
    const CTxDestination dest{DecodeDestination(address.get_str())};
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid ApertureCoin address: " + address.get_str());
    }
    return GetScriptForDestination(dest);
}

} // namespace

RPCHelpMan tokengenesis()
{
    return RPCHelpMan{"tokengenesis",
        "\nCreate a new native token category and send its initial supply and/or NFT to an address.\n"
        "The category ID is the txid of the wallet outpoint (with index 0) spent by the genesis input.\n"
        "If the wallet has no such outpoint, one is created first with a transaction to itself." +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "Recipient of the new tokens"},
            {"amount", RPCArg::Type::NUM, /* default */ "0", "Fungible supply to create (integer base units, up to 9223372036854775807)"},
            {"nft", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "Optionally create an NFT",
                {
                    {"capability", RPCArg::Type::STR, /* default */ "none", "none, mutable or minting"},
                    {"commitment", RPCArg::Type::STR_HEX, /* default */ "", "Up to 40 bytes"},
                },
            },
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "txid", "The genesis transaction id"},
                {RPCResult::Type::STR_HEX, "category", "The new token category"},
                {RPCResult::Type::STR_AMOUNT, "fee", "Fee paid"},
            }},
        RPCExamples{
            HelpExampleCli("tokengenesis", "\"" + EXAMPLE_ADDRESS[0] + "\" 1000000") +
            HelpExampleCli("-named tokengenesis", "address=\"" + EXAMPLE_ADDRESS[0] + "\" nft='{\"capability\":\"minting\"}'")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet& w{*wallet};
    w.BlockUntilSyncedToCurrentChain();
    LOCK(w.cs_wallet);

    const CScript recipient_script{LockingBytecodeForAddress(request.params[0])};
    token::TokenData td;
    if (!request.params[1].isNull()) {
        int64_t amount;
        if (!ParseInt64(request.params[1].getValStr(), &amount) || amount < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "amount must be a non-negative integer");
        }
        td.amount = amount;
    }
    if (!request.params[2].isNull()) {
        UniValue token_json(UniValue::VOBJ);
        token_json.pushKV("category", uint256().GetHex());
        token_json.pushKV("nft", request.params[2]);
        td.nft = ParseTokenData(token_json).nft;
    }
    std::string error;
    if (!token::IsValid(td, &error)) throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid token data: " + error);

    // Find a spendable, non-token wallet outpoint with index 0.
    std::optional<COutPoint> genesis_outpoint;
    {
        std::vector<COutput> available;
        w.AvailableCoins(available);
        for (const COutput& out : available) {
            if (!out.fSpendable) continue;
            if (out.i == 0) {
                genesis_outpoint = COutPoint(out.tx->GetHash(), 0);
                break;
            }
        }
    }
    if (!genesis_outpoint) {
        // Create one: pay ourselves with the recipient output at index 0.
        CCoinControl prep_control;
        std::vector<CRecipient> prep{{NewChangeScript(w), COIN / 100, false}};
        EnsureWalletIsUnlocked(&w);
        CAmount fee{0};
        int change_pos{1};
        bilingual_str prep_error;
        CTransactionRef prep_tx;
        FeeCalculation fee_calc;
        if (!w.CreateTransaction(prep, prep_tx, fee, change_pos, prep_error, prep_control, fee_calc, true)) {
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, prep_error.original);
        }
        w.CommitTransaction(prep_tx, {}, {});
        genesis_outpoint = COutPoint(prep_tx->GetHash(), 0);
    }

    td.category = genesis_outpoint->hash;
    CCoinControl coin_control;
    coin_control.Select(*genesis_outpoint);
    coin_control.fAllowOtherInputs = true;
    std::vector<CRecipient> recipients{TokenRecipient(w, td, recipient_script)};
    UniValue result{CreateAndCommit(w, recipients, coin_control)};
    result.pushKV("category", td.category.GetHex());
    return result;
},
    };
}

RPCHelpMan sendtoken()
{
    return RPCHelpMan{"sendtoken",
        "\nSend fungible tokens and/or one NFT of a category. Token change returns to the wallet;\n"
        "fees are paid in SCIENCE from the wallet." +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "Recipient"},
            {"category", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Token category"},
            {"amount", RPCArg::Type::NUM, RPCArg::Optional::NO, "Fungible amount to send (integer base units, may be 0 when sending an NFT)"},
            {"nft_commitment", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED_NAMED_ARG, "Also send the wallet's NFT of this category with this commitment (\"\" for an empty commitment)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                {RPCResult::Type::STR_AMOUNT, "fee", "Fee paid"},
            }},
        RPCExamples{HelpExampleCli("sendtoken", "\"" + EXAMPLE_ADDRESS[0] + "\" \"<category>\" 100")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet& w{*wallet};
    w.BlockUntilSyncedToCurrentChain();
    LOCK(w.cs_wallet);

    const CScript recipient_script{LockingBytecodeForAddress(request.params[0])};
    const uint256 category{ParseHashV(request.params[1], "category")};
    int64_t amount;
    if (!ParseInt64(request.params[2].getValStr(), &amount) || amount < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "amount must be a non-negative integer");
    }
    std::optional<std::vector<unsigned char>> nft_commitment;
    if (!request.params[3].isNull()) nft_commitment = ParseHexV(request.params[3], "nft_commitment");
    if (amount == 0 && !nft_commitment) throw JSONRPCError(RPC_INVALID_PARAMETER, "nothing to send");

    std::vector<TokenCoin> coins;
    for (TokenCoin& coin : ListTokenCoins(w)) {
        if (coin.token_data.category == category) coins.push_back(std::move(coin));
    }

    // Select the NFT first (if requested), then fungible-only coins, then
    // fungible amounts carried by other NFT coins.
    std::vector<const TokenCoin*> selected;
    std::optional<token::NFT> sent_nft;
    if (nft_commitment) {
        for (const TokenCoin& coin : coins) {
            if (coin.token_data.nft && coin.token_data.nft->commitment == *nft_commitment) {
                selected.push_back(&coin);
                sent_nft = coin.token_data.nft;
                break;
            }
        }
        if (!sent_nft) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "NFT not found in wallet");
    }
    int64_t selected_amount{0};
    for (const TokenCoin* coin : selected) selected_amount += coin->token_data.amount;
    for (int pass = 0; pass < 2 && selected_amount < amount; ++pass) {
        for (const TokenCoin& coin : coins) {
            if (selected_amount >= amount) break;
            if (coin.token_data.amount == 0 || std::find(selected.begin(), selected.end(), &coin) != selected.end()) continue;
            if ((pass == 0) == coin.token_data.nft.has_value()) continue; // pass 0: fungible-only coins
            selected.push_back(&coin);
            selected_amount += coin.token_data.amount;
        }
    }
    if (selected_amount < amount) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strprintf("Insufficient token balance: %d < %d", selected_amount, amount));
    }

    std::vector<CRecipient> recipients;
    token::TokenData out_td;
    out_td.category = category;
    out_td.amount = amount;
    out_td.nft = sent_nft;
    recipients.push_back(TokenRecipient(w, out_td, recipient_script));

    // Token change: remaining fungible amount, plus every NFT on a selected
    // coin other than the one sent (so nothing is burned).
    const CScript change_script{NewChangeScript(w)};
    int64_t change_amount{selected_amount - amount};
    bool nft_sent_accounted{false};
    for (const TokenCoin* coin : selected) {
        if (!coin->token_data.nft) continue;
        if (sent_nft && !nft_sent_accounted && coin->token_data.nft == sent_nft) {
            nft_sent_accounted = true;
            continue;
        }
        token::TokenData keep;
        keep.category = category;
        keep.nft = coin->token_data.nft;
        keep.amount = change_amount; // attach the fungible change to the first kept NFT
        change_amount = 0;
        recipients.push_back(TokenRecipient(w, keep, change_script));
    }
    if (change_amount > 0) {
        token::TokenData change;
        change.category = category;
        change.amount = change_amount;
        recipients.push_back(TokenRecipient(w, change, change_script));
    }

    CCoinControl coin_control;
    for (const TokenCoin* coin : selected) coin_control.Select(coin->outpoint);
    coin_control.fAllowOtherInputs = true;
    return CreateAndCommit(w, recipients, coin_control);
},
    };
}

RPCHelpMan listtokens()
{
    return RPCHelpMan{"listtokens",
        "\nList the wallet's spendable native token outputs and balances per category.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::OBJ_DYN, "balances", "Fungible balance and NFT count per category",
                    {
                        {RPCResult::Type::OBJ, "category", "",
                            {
                                {RPCResult::Type::STR, "amount", "Fungible balance (integer as string)"},
                                {RPCResult::Type::NUM, "nfts", "Number of NFTs"},
                            }},
                    }},
                {RPCResult::Type::ARR, "outputs", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", ""},
                                {RPCResult::Type::NUM, "vout", ""},
                                {RPCResult::Type::NUM, "confirmations", ""},
                                {RPCResult::Type::STR_AMOUNT, "value", "SCIENCE carried by the output"},
                                {RPCResult::Type::OBJ, "tokenData", "", {{RPCResult::Type::ELISION, "", ""}}},
                            }},
                    }},
            }},
        RPCExamples{HelpExampleCli("listtokens", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    wallet->BlockUntilSyncedToCurrentChain();
    LOCK(wallet->cs_wallet);

    std::map<uint256, std::pair<int64_t, int64_t>> balances;
    UniValue outputs(UniValue::VARR);
    for (const TokenCoin& coin : ListTokenCoins(*wallet)) {
        auto& [amount, nfts] = balances[coin.token_data.category];
        amount += coin.token_data.amount;
        nfts += coin.token_data.nft ? 1 : 0;
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("txid", coin.outpoint.hash.GetHex());
        entry.pushKV("vout", (int)coin.outpoint.n);
        entry.pushKV("confirmations", coin.depth);
        entry.pushKV("value", ValueFromAmount(coin.txout.nValue));
        entry.pushKV("tokenData", TokenDataToUniv(coin.token_data));
        outputs.push_back(entry);
    }
    UniValue balances_json(UniValue::VOBJ);
    for (const auto& [category, balance] : balances) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("amount", std::to_string(balance.first));
        entry.pushKV("nfts", balance.second);
        balances_json.pushKV(category.GetHex(), entry);
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("balances", balances_json);
    result.pushKV("outputs", outputs);
    return result;
},
    };
}

// ---------------------------------------------------------------------------
// Embedding requests (ApertureMatMul v2, doc/pouw-v2.md)

RPCHelpMan sendembeddingrequest()
{
    return RPCHelpMan{"sendembeddingrequest",
        "\nPay miners to embed an input with the protocol model. The request is served, with the exact\n"
        "protocol-model embedding, by the block that includes it (see getblockembeddings)." +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"input", RPCArg::Type::STR, RPCArg::Optional::NO, "Text (byte-tokenizer models) or a JSON array of token ids"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "txid", "Request transaction"},
                {RPCResult::Type::NUM, "vout", "Request output"},
                {RPCResult::Type::NUM, "tokens", "Token ids requested"},
                {RPCResult::Type::STR_AMOUNT, "fee", "Fee paid"},
            }},
        RPCExamples{HelpExampleCli("sendembeddingrequest", "\"hello world\"") + HelpExampleCli("sendembeddingrequest", "\"[9707, 1879]\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet& w{*wallet};
    w.BlockUntilSyncedToCurrentChain();

    const intmodel::IntModel* model = embed::GetProtocolModel();
    if (!model) throw JSONRPCError(RPC_MISC_ERROR, "No protocol model loaded (ApertureMatMul v2 inactive)");
    std::vector<uint32_t> ids;
    const UniValue& in = request.params[0];
    UniValue parsed;
    if (in.isStr() && parsed.read(in.get_str()) && parsed.isArray()) {
        for (size_t k = 0; k < parsed.size(); ++k) ids.push_back(static_cast<uint32_t>(parsed[k].get_int64()));
    } else if (in.isArray()) {
        for (size_t k = 0; k < in.size(); ++k) ids.push_back(static_cast<uint32_t>(in[k].get_int64()));
    } else if (in.isStr() && model->Config().tokenizer == "bytes") {
        for (unsigned char c : in.get_str()) ids.push_back(c);
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "This model needs an array of token ids");
    }
    if (embed::ModelInput(*model, ids).empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "input cannot be served by the protocol model");
    const unsigned int budget = Params().GetConsensus().nMaxEmbedTokens;
    if (ids.size() + 1 > budget) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("bad-embed-tokens: the input has %u tokens with EOS, a block serves at most %u", ids.size() + 1, budget));
    }

    LOCK(w.cs_wallet);
    EnsureWalletIsUnlocked(&w);
    std::vector<CRecipient> recipients{{embed::MakeRequestScript(ids), 0, false}};
    CCoinControl coin_control;
    CAmount fee{0};
    int change_pos{-1};
    bilingual_str error;
    CTransactionRef tx;
    FeeCalculation fee_calc;
    if (!w.CreateTransaction(recipients, tx, fee, change_pos, error, coin_control, fee_calc, true)) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, error.original);
    }
    w.CommitTransaction(tx, {}, {});
    UniValue out(UniValue::VOBJ);
    out.pushKV("txid", tx->GetHash().GetHex());
    for (uint32_t n = 0; n < tx->vout.size(); ++n) {
        if (embed::IsRequestScript(tx->vout[n].scriptPubKey)) out.pushKV("vout", (uint64_t)n);
    }
    out.pushKV("tokens", (uint64_t)ids.size());
    out.pushKV("fee", ValueFromAmount(fee));
    return out;
},
    };
}
