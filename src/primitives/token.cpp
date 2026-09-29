// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/token.h>

#include <serialize.h>
#include <streams.h>
#include <version.h>

#include <limits>

namespace token {
namespace {

bool Fail(std::string* error, const char* reason)
{
    if (error) *error = reason;
    return false;
}

} // namespace

bool IsValid(const TokenData& token_data, std::string* error)
{
    if (!token_data.nft && token_data.amount == 0) return Fail(error, "token-no-nft-or-amount");
    if (token_data.amount < 0) return Fail(error, "token-amount-negative");
    if (token_data.nft) {
        const auto cap = static_cast<uint8_t>(token_data.nft->capability);
        if (cap > static_cast<uint8_t>(Capability::MINTING)) return Fail(error, "token-bad-capability");
        if (token_data.nft->commitment.size() > MAX_COMMITMENT_LENGTH) return Fail(error, "token-commitment-oversize");
    }
    return true;
}

ParseResult Parse(const CScript& script_pub_key, TokenData& token_data, CScript* locking_bytecode, std::string* error)
{
    if (!HasTokenPrefix(script_pub_key)) return ParseResult::NO_TOKEN;
    const auto invalid = [&](const char* reason) {
        if (error) *error = reason;
        return ParseResult::INVALID;
    };
    // Minimum: prefix + category + bitfield.
    if (script_pub_key.size() < 1 + 32 + 1) return invalid("token-prefix-truncated");

    TokenData out;
    std::copy(script_pub_key.begin() + 1, script_pub_key.begin() + 33, out.category.begin());
    const uint8_t bitfield{script_pub_key[33]};
    if (bitfield & RESERVED_BIT) return invalid("token-reserved-bit");
    const uint8_t capability{static_cast<uint8_t>(bitfield & CAPABILITY_MASK)};
    const bool has_nft{(bitfield & HAS_NFT) != 0};
    const bool has_commitment{(bitfield & HAS_COMMITMENT_LENGTH) != 0};
    const bool has_amount{(bitfield & HAS_AMOUNT) != 0};
    if (!has_nft && !has_amount) return invalid("token-no-nft-or-amount");
    if (!has_nft && (has_commitment || capability != 0)) return invalid("token-nft-flags-without-nft");
    if (capability > static_cast<uint8_t>(Capability::MINTING)) return invalid("token-bad-capability");

    std::vector<unsigned char> rest(script_pub_key.begin() + 34, script_pub_key.end());
    VectorReader reader(SER_NETWORK, PROTOCOL_VERSION, rest, 0);
    try {
        if (has_nft) {
            NFT nft;
            nft.capability = static_cast<Capability>(capability);
            if (has_commitment) {
                const uint64_t len{ReadCompactSize(reader, /*range_check=*/false)};
                if (len == 0) return invalid("token-commitment-empty");
                if (len > MAX_COMMITMENT_LENGTH) return invalid("token-commitment-oversize");
                nft.commitment.resize(len);
                reader.read(reinterpret_cast<char*>(nft.commitment.data()), len);
            }
            out.nft = std::move(nft);
        }
        if (has_amount) {
            const uint64_t amount{ReadCompactSize(reader, /*range_check=*/false)};
            if (amount == 0) return invalid("token-amount-zero");
            if (amount > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return invalid("token-amount-overflow");
            out.amount = static_cast<int64_t>(amount);
        }
    } catch (const std::ios_base::failure&) {
        // Truncated or non-canonical CompactSize.
        return invalid("token-prefix-malformed");
    }

    if (locking_bytecode) {
        const size_t consumed{rest.size() - reader.size()};
        locking_bytecode->assign(script_pub_key.begin() + 34 + consumed, script_pub_key.end());
    }
    token_data = std::move(out);
    return ParseResult::OK;
}

CScript GetLockingBytecode(const CScript& script_pub_key)
{
    if (!HasTokenPrefix(script_pub_key)) return script_pub_key;
    TokenData token_data;
    CScript locking_bytecode;
    if (Parse(script_pub_key, token_data, &locking_bytecode) != ParseResult::OK) return script_pub_key;
    return locking_bytecode;
}

CScript Encode(const TokenData& token_data, const CScript& locking_bytecode)
{
    std::vector<unsigned char> out;
    out.push_back(PREFIX_TOKEN);
    out.insert(out.end(), token_data.category.begin(), token_data.category.end());
    uint8_t bitfield{0};
    if (token_data.nft) {
        bitfield |= HAS_NFT | static_cast<uint8_t>(token_data.nft->capability);
        if (!token_data.nft->commitment.empty()) bitfield |= HAS_COMMITMENT_LENGTH;
    }
    if (token_data.amount > 0) bitfield |= HAS_AMOUNT;
    out.push_back(bitfield);
    CVectorWriter writer(SER_NETWORK, PROTOCOL_VERSION, out, out.size());
    if (token_data.nft && !token_data.nft->commitment.empty()) {
        WriteCompactSize(writer, token_data.nft->commitment.size());
        writer.write(reinterpret_cast<const char*>(token_data.nft->commitment.data()), token_data.nft->commitment.size());
    }
    if (token_data.amount > 0) WriteCompactSize(writer, static_cast<uint64_t>(token_data.amount));
    out.insert(out.end(), locking_bytecode.begin(), locking_bytecode.end());
    return CScript(out.begin(), out.end());
}

std::string CapabilityToString(Capability capability)
{
    switch (capability) {
    case Capability::NONE: return "none";
    case Capability::MUTABLE: return "mutable";
    case Capability::MINTING: return "minting";
    }
    return "unknown";
}

std::optional<Capability> CapabilityFromString(const std::string& str)
{
    if (str == "none") return Capability::NONE;
    if (str == "mutable") return Capability::MUTABLE;
    if (str == "minting") return Capability::MINTING;
    return std::nullopt;
}

} // namespace token
