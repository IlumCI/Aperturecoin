// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_TOKEN_H
#define BITCOIN_PRIMITIVES_TOKEN_H

#include <amount.h>
#include <script/script.h>
#include <uint256.h>

#include <optional>
#include <string>
#include <vector>

/**
 * Native tokens (see doc/tokens.md). Semantics follow the CashTokens
 * specification (CHIP-2022-02): fungible amounts and non-fungible tokens
 * (NFTs) are attached to ordinary outputs. The token data is encoded as a
 * prefix of the output's scriptPubKey field:
 *
 *   PREFIX_TOKEN (0xef) || category (32 bytes) || bitfield (1 byte)
 *   || [commitment length (CompactSize) || commitment]   if HAS_COMMITMENT_LENGTH
 *   || [fungible amount (CompactSize)]                   if HAS_AMOUNT
 *   || locking bytecode (the actual script)
 *
 * The transaction serialization is unchanged, and signature hashes that commit
 * to spent scriptPubKeys (BIP341) commit to the token data.
 */
namespace token {

static constexpr uint8_t PREFIX_TOKEN{0xef};
static constexpr size_t MAX_COMMITMENT_LENGTH{40};

// Bitfield flags (high nibble) and capability (low nibble).
static constexpr uint8_t RESERVED_BIT{0x80};
static constexpr uint8_t HAS_COMMITMENT_LENGTH{0x40};
static constexpr uint8_t HAS_NFT{0x20};
static constexpr uint8_t HAS_AMOUNT{0x10};
static constexpr uint8_t CAPABILITY_MASK{0x0f};

enum class Capability : uint8_t {
    NONE = 0,    //!< immutable NFT
    MUTABLE = 1, //!< may be replaced by one NFT with any commitment
    MINTING = 2, //!< may create any number of NFTs of its category
};

struct NFT {
    Capability capability{Capability::NONE};
    std::vector<unsigned char> commitment;

    friend bool operator==(const NFT& a, const NFT& b) { return a.capability == b.capability && a.commitment == b.commitment; }
};

struct TokenData {
    /** Category ID: the txid (internal byte order) of the outpoint spent by the genesis input. */
    uint256 category;
    std::optional<NFT> nft;
    /** Fungible amount; 0 when the output carries no fungible tokens. */
    int64_t amount{0};

    friend bool operator==(const TokenData& a, const TokenData& b)
    {
        return a.category == b.category && a.nft == b.nft && a.amount == b.amount;
    }
};

enum class ParseResult {
    NO_TOKEN, //!< scriptPubKey does not start with PREFIX_TOKEN
    OK,
    INVALID,  //!< starts with PREFIX_TOKEN but is not a valid token prefix
};

/** True if the scriptPubKey field starts with PREFIX_TOKEN. */
inline bool HasTokenPrefix(const CScript& script_pub_key)
{
    return !script_pub_key.empty() && script_pub_key[0] == PREFIX_TOKEN;
}

/**
 * Parse the token prefix of a scriptPubKey field. On OK, token_data holds the
 * token data and, if locking_bytecode is non-null, it receives the script
 * that follows the prefix.
 */
ParseResult Parse(const CScript& script_pub_key, TokenData& token_data, CScript* locking_bytecode = nullptr, std::string* error = nullptr);

/** Return the locking bytecode: the script without its token prefix (unchanged if there is none or it is invalid). */
CScript GetLockingBytecode(const CScript& script_pub_key);

/** Encode token data in front of a locking bytecode. token_data must be valid (see IsValid). */
CScript Encode(const TokenData& token_data, const CScript& locking_bytecode);

/** Structural validity of token data (the same rules Parse enforces). */
bool IsValid(const TokenData& token_data, std::string* error = nullptr);

std::string CapabilityToString(Capability capability);
std::optional<Capability> CapabilityFromString(const std::string& str);

} // namespace token

#endif // BITCOIN_PRIMITIVES_TOKEN_H
