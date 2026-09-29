Native tokens
=============

ApertureCoin supports fungible tokens and non-fungible tokens (NFTs) at the
consensus layer. The semantics follow the CashTokens specification
(CHIP-2022-02, live on Bitcoin Cash since 2023). The implementation here is
written from that specification.

Tokens are attached to ordinary transaction outputs, so every node enforces
the token rules. There are no indexers or off-chain balances.

Encoding
--------

Token data is a prefix of the output's scriptPubKey field:

```
0xef || category (32 bytes) || bitfield (1 byte)
     || [commitment length (CompactSize) || commitment]   if HAS_COMMITMENT_LENGTH (0x40)
     || [fungible amount (CompactSize)]                   if HAS_AMOUNT (0x10)
     || locking bytecode
```

| Bitfield bits | Meaning |
|---|---|
| 0x80 | Reserved. Must be 0. |
| 0x40 | Has a commitment (1–40 bytes). Requires 0x20. |
| 0x20 | Has an NFT. |
| 0x10 | Has a fungible amount (1 … 2^63−1). |
| 0x0f | NFT capability: 0 none (immutable), 1 mutable, 2 minting. Must be 0 without an NFT. |

Every output needs an NFT, a fungible amount, or both. CompactSize values
must be canonical.

- **Serialization.** The transaction serialization is unchanged.
- **Script evaluation.** Scripts evaluate the locking bytecode behind the
  prefix. `VerifyScript` and `Solver` strip the prefix, and so do sigop
  counting, dust and standardness checks.
- **Taproot signatures.** Taproot (BIP341) signatures commit to all spent
  scriptPubKeys, and therefore to the token data.
- **SegWit v0 signatures.** Segwit v0 (BIP143) signatures commit only to the
  input's own script code and amount. Hardware signers should therefore
  prefer Taproot for token inputs.

Consensus rules
---------------

The context-free checks are in `CheckTransaction`; the rest are in
`CheckTxInputs`.

1. **Well-formed prefixes.** Every output prefix must be valid
   (`bad-txns-token-prefix`). Coinbase outputs cannot carry tokens
   (`bad-txns-coinbase-token`).
2. **Genesis.** A new category can only be created by a transaction that
   spends an outpoint with index 0. The category ID is that outpoint's txid.
   Any fungible supply and any NFTs may be created at genesis.
3. **Known categories.** Every output category must appear in an input or be
   a genesis category (`bad-txns-token-category`).
4. **Fungible conservation.** For each non-genesis category, the fungible
   amount in the outputs must not exceed the amount in the inputs
   (`bad-txns-token-amount-inflation`). Burning is allowed.
5. **NFTs.**
   - A **minting** NFT in the inputs (or genesis) allows any NFTs of its
     category.
   - Minting capability cannot be created from anything else
     (`bad-txns-token-nft-minting`).
   - An **immutable** NFT can only pass through unchanged.
   - A **mutable** NFT can be replaced by one NFT with any commitment and
     capability none or mutable.
   - An NFT that no input justifies is rejected
     (`bad-txns-token-nft-ex-nihilo`).

These rules are active from genesis on every network.

RPC
---

| RPC | Purpose |
|---|---|
| `tokengenesis address (amount) ({"capability","commitment"})` | Create a category. If the wallet has no outpoint with index 0, it first creates one by paying itself. |
| `sendtoken address category amount ("nft_commitment")` | Send fungible tokens and/or one NFT. Token change and any other NFTs on the spent coins return to the wallet. Fees are paid in SCIENCE. |
| `listtokens` | Balances per category, and token UTXOs. |
| `createrawtransaction` | Output values may be `{"amount": x, "token": {"category", "amount", "nft": {"capability", "commitment"}}}`. |
| `decoderawtransaction` / `getrawtransaction` / `getblock` | Outputs carry a `tokenData` object. |

The wallet never uses token outputs for ordinary payments. Spending them would
burn the tokens, so coin selection only takes a token output when it has been
explicitly selected.

Test coverage
-------------

| Test | What it covers |
|---|---|
| `src/test/token_tests.cpp` | Encoding round trips, a byte-exact vector, malformed prefixes, and prefix stripping in `Solver`/`VerifyScript`. |
| `test/functional/feature_tokens.py` | Each consensus rule above, including rejection reasons and the coinbase restriction. |
| `test/functional/wallet_tokens.py` | The RPCs, NFT transfer, and the guarantee that ordinary payments never touch token outputs. |
