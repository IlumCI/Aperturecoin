// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_FRAUDCLAIM_H
#define BITCOIN_CONSENSUS_FRAUDCLAIM_H

#include <primitives/transaction.h>
#include <script/script.h>

#include <cstring>

/**
 * A fraud claim (doc/pouw-v2.md, "Fraud proofs") is a transaction with an
 * OP_RETURN "APFP" output. It spends the coinbase outputs of the block whose
 * embedding result it proves wrong. Such transactions are exempt from
 * coinbase maturity and from script checks, and must pass CheckFraudClaim()
 * (validation.cpp) instead, in the mempool and in every block.
 */
inline bool IsFraudClaimScript(const CScript& s)
{
    static const unsigned char TAG[4] = {'A', 'P', 'F', 'P'};
    return s.size() >= 6 && s[0] == OP_RETURN && s[1] == 4 && memcmp(&s[2], TAG, 4) == 0;
}

inline bool IsFraudClaimTx(const CTransaction& tx)
{
    if (tx.IsCoinBase()) return false;
    for (const CTxOut& out : tx.vout) {
        if (IsFraudClaimScript(out.scriptPubKey)) return true;
    }
    return false;
}

#endif // BITCOIN_CONSENSUS_FRAUDCLAIM_H
