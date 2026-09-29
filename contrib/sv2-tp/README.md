Stratum V2 for ApertureCoin
===========================

Stratum V2 moves block-template construction from the pool to the miner. A
miner runs its own `apertured` and a **Template Provider**. The Template
Provider builds templates from the miner's own mempool and sends them over an
authenticated, encrypted channel (Noise NX, secp256k1 + ElligatorSwift,
ChaCha20-Poly1305). A pool can then only pay the miner; it cannot choose which
transactions get mined.

This directory builds two binaries:

| Binary | Role |
|---|---|
| `sv2-tp` | Template Provider. It is [stratum-mining/sv2-tp](https://github.com/stratum-mining/sv2-tp) (MIT) at the commit in `UPSTREAM_COMMIT`, plus a JSON-RPC backend for `apertured`. ApertureCoin Core 0.21 has no Mining IPC interface. |
| `aperture-sv2-miner` | Reference SV2 CPU miner. It speaks the Template Distribution Protocol directly to `sv2-tp`, builds its own coinbase, grinds ApertureMatMul and submits solutions. |

Build
-----

```
sudo apt-get install cmake capnproto libcapnp-dev libboost-dev
contrib/sv2-tp/build.sh
```

The build script clones upstream at the pinned commit, applies
`patches/0001-aperture-rpc-backend-and-miner.patch`, copies `overlay/`, and
builds with `-DAPERTURE_SRC=<this repository>`. The miner compiles
`src/crypto/matmulpow.cpp` and BLAKE3 from this repository, so it always
matches consensus.

Run
---

1. **Start the node:**
   ```
   apertured -daemon
   ```
2. **Start the Template Provider.** Create its data directory first:
   ```
   mkdir -p ~/.sv2-tp
   sv2-tp -datadir=$HOME/.sv2-tp -rpccookiefile=$HOME/.aperture/.cookie -sv2port=8442
   ```
   The log prints `Template Provider authority key: <base58>`. The key is
   stable across restarts; it is stored in `sv2_authority_key`.
3. **Mine solo over SV2:**
   ```
   aperture-sv2-miner -connect=127.0.0.1:8442 -authority=<base58> \
       -payout=<your scriptPubKey hex> -dim=512 -threads=$(nproc)
   ```
   Use `-dim=32` on regtest.

For pooled mining, point the SRI Job Declarator Client or Pool at `sv2-tp`
the same way. The SRI pool and proxy roles also need ApertureMatMul share
validation; that fork is tracked separately (see `doc/stratum-v2.md`).

Changes relative to upstream
----------------------------

- **RPC backend.** New options `-rpcconnect/-rpcport/-rpcuser/-rpcpassword/-rpccookiefile`
  select `aperture::RpcMining` (`overlay/src/aperture/rpc_mining.cpp`) instead
  of IPC. Templates come from `getblocktemplate`, and solutions go out with
  `submitblock`. The tip and fees are polled every 250 ms and 2 s
  respectively.
- **Coinbase outputs.** In each template, output 0 is the reward placeholder.
  Every other output is a **required** coinbase output: the development fund
  (a non-`OP_RETURN` P2WSH) and the witness commitment. Upstream forwarded only
  `OP_RETURN` outputs, which would have dropped the consensus-required
  development fund payment.
- **`aperture-sv2-miner`.** A reference CPU miner (new file).

Tests
-----

- `test_sv2`: the upstream unit tests (Noise, transport, messages, template
  provider with a mock backend).
- End to end, on regtest:
  1. Start `apertured -regtest -devfundendheight=1000`.
  2. Start `sv2-tp -regtest -rpccookiefile=...`.
  3. Run `aperture-sv2-miner -dim=32 -blocks=5`.

  The node's height advances by 5. Each coinbase pays the miner's payout
  output, 0.75 SCIENCE to the development fund, and the witness commitment.
