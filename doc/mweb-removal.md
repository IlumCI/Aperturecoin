MWEB removal
============

ApertureCoin inherited the MimbleWimble Extension Block (MWEB, LIP-0002/3/4/6)
from Litecoin Core 0.21.5.x. It was never active on mainnet or testnet
(`NEVER_ACTIVE`), only on regtest. It has been removed before launch, so no
chain state has to be migrated.

What changed
------------

- Consensus and serialization: transactions, blocks, block index entries,
  coins and undo data use the Bitcoin Core 0.21 formats again. There is no
  HogEx transaction, no extension block, no peg-in/peg-out and no
  `SERIALIZE_NO_MWEB` flag. `Coin` has no peg-out bit.
- P2P: `PROTOCOL_VERSION` is 70016, `NODE_MWEB` and the MWEB messages
  (`mwebheader`, `mwebleafset`, `getmwebutxos`, `mwebutxos`) are gone, compact
  blocks are version 1/2 (BIP152), and `MAX_PROTOCOL_MESSAGE_LENGTH` is 4 MB.
- RPC: `mweb` fields in block, transaction and template output are gone, and
  there is no `mweb` address type or `getblocktemplate` rule. Unknown template
  rules are ignored, so clients that still request `mweb` keep working.
- Wallet and GUI: the wallet uses the Bitcoin Core 0.21.1 `CWallet`
  (coin selection, transaction creation and records), with ApertureCoin tokens
  on top. The MWEB keychain, stealth addresses, `txassembler` and
  `listwallettransactions` have been removed.
- Build: `src/libmw`, `src/mweb` and the `libfmt` dependency are gone. BLAKE3
  (used by ApertureMatMul) is compiled in `src/crypto/blake3_unit.cpp`.
  `secp256k1-zkp` stays in the tree, built with the recovery, schnorrsig and
  extrakeys modules.

Litecoin changes kept (not part of MWEB)
----------------------------------------

- Address relay rate limiting and its statistics (`addr_processed`,
  `addr_rate_limited`).
- `addconnection`, and `MarkBlockAsReceived` peer attribution.
- The mempool `CompareDepthAndScore` fix, the script-check queue ordering fix
  in `ConnectBlock`, and the hardening of compact-block reconstruction.
- Litecoin fee and dust defaults, and `-peerblockfilters` on by default.

Privacy direction
-----------------

Confidential amounts on an extension block cost a second consensus state and
a second wallet. Privacy for ApertureCoin will be revisited as a tapscript- or
token-level design on the single UTXO set, not by restoring MWEB.
