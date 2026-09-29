Development fund key ceremony
=============================

The development fund receives 1.5% of the block subsidy (0.75 SCIENCE per
block) until the first halving at height 1,051,200. Over that period it totals
788,400 SCIENCE. Consensus enforces the output: see `GetDevFundAmount()` in
`src/validation.cpp` and `devFundScript` in `src/chainparams.cpp`.

Until the keyholders are chosen, mainnet and testnet pay a provably
unspendable `OP_RETURN "DEVFUND"` script. Anything paid to it is burned. That
placeholder **must** be replaced before the mainnet genesis block is re-mined.

Target script
-------------

A 2-of-3 P2WSH multisig with sorted keys:

```
wsh(sortedmulti(2, <xpub_A>/0/0, <xpub_B>/0/0, <xpub_C>/0/0))
```

A 3-of-5 variant uses the same procedure with five holders.

Procedure
---------

1. **Choose holders.** Pick three independent keyholders. If possible, use
   different people, different jurisdictions and different hardware vendors.
2. **Generate keys offline.** Each holder generates a BIP39 seed on an
   air-gapped device or hardware wallet and derives the account key at
   `m/48'/0'/0'/2'`. Holders keep their seed backup in two separate physical
   locations.
3. **Exchange public keys.** Each holder publishes their xpub with a detached
   signature from a key already known publicly (PGP, or a signed message from
   a published address).
4. **Build the descriptor.** Every holder independently constructs the
   descriptor with `getdescriptorinfo` and derives the P2WSH script with
   `deriveaddresses`. All holders must report the same script hex.
5. **Test spend on testnet.** Put the same descriptor on testnet, mine to it
   (`-devfundendheight` is available only on regtest, so use a testnet build
   that carries the script), and complete one 2-of-3 PSBT spend with
   `walletcreatefundedpsbt`, `walletprocesspsbt` and `finalizepsbt`.
6. **Update chainparams.** Replace `DevFundPlaceholderScript()` for mainnet
   (and testnet, if it will use the same keys) with the script bytes. Update
   `README.md` and this document with the descriptor, without any private
   material.
7. **Re-mine genesis.** Re-mine the mainnet genesis block
   (`contrib/genesis/README.md`), then update the asserts. The script change
   does not alter the genesis block itself, but it must land in the release
   tag that is published together with the genesis timestamp.
8. **Publish.** Publish the descriptor, the three xpubs and the holders'
   signatures in the release notes of the first mainnet release.

Spending policy
---------------

Spends go through PSBT and need two of three signatures. Publish every
spend with a short justification. The fund's address is public, so anyone can
audit it.
