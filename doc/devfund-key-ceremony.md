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

A 2-of-3 Taproot script-path multisig:

```
tr(H, sortedmulti_a(2, A, B, C))
```

- `H` is the BIP341 internal key with no known discrete logarithm
  (`50929b74…803ac0`), so there is no key-path spend.
- The single leaf is
  `<K1> OP_CHECKSIG <K2> OP_CHECKSIGADD <K3> OP_CHECKSIGADD OP_2 OP_NUMEQUAL`,
  with K1 < K2 < K3 the holders' x-only public keys in byte order.
- Compared with P2WSH `sortedmulti`, a spend carries 64-byte BIP340
  signatures and reveals only the leaf that is used, and Taproot is active
  from genesis.

`contrib/devfund/devfund.py` builds the script, the address, the descriptor
and the chainparams line, and runs the spend flow. A 3-of-5 variant is
`devfund.py script A B C D E --threshold=3`.

If you are the only holder
--------------------------

Until other people join, all three keys can belong to the project owner.
2-of-3 then still protects against losing one device or one backup, and
against one stolen device, but not against the owner. Recommended setup:

- three keys on three separate devices or offline storage media, in at least
  two physical places;
- `devfund.py keygen -o key-N.json` on each, run while the device is offline;
- only the `xonly_pubkey` values leave those devices.

A key can be handed to an independent holder later only by moving the fund
to a new script (a spend to the new 2-of-3 output for funds already received,
and a release that changes `devFundScript` for future blocks). Choosing
independent holders before mainnet avoids that.

Never generate holder keys on a shared or online machine, and never send a
private key to anyone, including this project's tooling or its assistants.

Procedure
---------

1. **Choose holders.** Pick three independent keyholders. If possible, use
   different people, different jurisdictions and different hardware.
2. **Generate keys offline.** Each holder runs `devfund.py keygen -o key.json`
   on an air-gapped machine (or uses a signer that supports Taproot script
   paths), backs the key up in two separate physical locations, and records
   the 32-byte x-only public key.
3. **Exchange public keys.** Each holder publishes their x-only key with a
   detached signature from a key already known publicly (PGP, or a signed
   message from a published address).
4. **Build the script.** Every holder independently runs
   `contrib/devfund/devfund.py script A B C` and checks that all holders get
   the same `script_pubkey`.
5. **Test spend on regtest and testnet.**
   - Regtest: `apertured -regtest -devfundscript=<script_pubkey> -devfundendheight=1000`,
     mine, let the outputs mature, then spend two of them with
     `devfund.py create`, `sign` (two holders) and `finalize`.
     `test/functional/feature_devfund_multisig.py` runs exactly this.
   - Testnet: put the script in a testnet build and repeat one spend.
6. **Update chainparams.** Replace `DevFundPlaceholderScript()` for mainnet
   (and testnet, if it uses the same keys) with the `chainparams` line from
   step 4. Update `README.md` and this document with the descriptor, without
   any private material.
7. **Re-mine genesis.** Re-mine the mainnet genesis block
   (`contrib/genesis/README.md`), then update the asserts. The script change
   does not alter the genesis block itself, but it must land in the release
   tag that is published together with the genesis timestamp.
8. **Publish.** Publish the descriptor, the three x-only keys and the
   holders' signatures in the release notes of the first mainnet release.

Spending
--------

A spend is a JSON session file passed between holders, like a PSBT:

```
devfund.py create A B C --utxo=TXID:VOUT:SATS ... --pay=SCRIPTHEX:SATS ... --fee=SATS -o spend.json
devfund.py sign spend.json --privkey=<holder 1>     # on holder 1's offline machine
devfund.py sign spend.json --privkey=<holder 2>     # on holder 2's offline machine
devfund.py finalize spend.json                      # prints the transaction hex
aperture-cli sendrawtransaction <hex>
```

Fund outputs are coinbase outputs and mature after 100 blocks. The Python
secp256k1 code in the tool is not constant-time: sign only on offline
machines.

Spending policy
---------------

Spends need two of three signatures. Publish every spend with a short
justification. The fund's address is public, so anyone can audit it.
