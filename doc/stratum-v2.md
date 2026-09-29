Stratum V2
==========

Under Stratum V1, the pool builds the block and miners only hash it, so pool
operators decide which transactions are mined. Stratum V2 separates hashing
from template construction:

```
apertured --RPC--> sv2-tp ==SV2 Template Distribution (Noise)==> aperture-sv2-miner   (solo)
                         ==SV2 Template Distribution==> Job Declarator Client ==> Pool  (pooled)
```

- **Template Provider (`sv2-tp`).** It runs next to the miner's own node, so
  templates come from the miner's own mempool and policy.
- **Encryption and authentication.** Connections use Noise NX over secp256k1
  with ElligatorSwift, and ChaCha20-Poly1305. The TP identifies itself with an
  authority key that clients pin.
- **Mandatory coinbase outputs.** Consensus requires the development fund
  output and the witness commitment, so templates send them as required
  coinbase outputs. The miner or pool adds only its own payout.

Components
----------

| Component | Status | Location |
|---|---|---|
| Template Provider | Done. Upstream `sv2-tp` plus a JSON-RPC backend for ApertureCoin Core 0.21 | `contrib/sv2-tp` |
| Reference CPU miner (solo, Template Distribution Protocol) | Done | `contrib/sv2-tp/overlay/src/aperture-sv2-miner.cpp` |
| End-to-end test | Done. Skipped unless `SV2TP`/`SV2MINER` are set | `test/functional/feature_sv2.py` |
| Pool, Job Declarator and Translator proxy (SRI `sv2-apps`) | Not started | Planned fork |
| GPU miner | Not started | Planned |

Porting the SRI pool
--------------------

The SV2 wire messages carry only header fields (version, prev_hash, merkle
root, ntime, nbits, nonce), so they do not depend on the hash function. An SRI
fork needs to change three things:

1. **Share and block validation.** Replace the header hash (rust-bitcoin
   `BlockHash`/SHA256d) with ApertureMatMul. Use a small Rust crate built on
   the `blake3` crate and an exact int8 matrix multiply, and validate it
   against the vectors in `src/test/matmulpow_tests.cpp`. Keep the block
   *identifier* as SHA256d.
2. **Difficulty and target math.** Replace it with the ApertureCoin
   `powLimit` for each network.
3. **Required coinbase outputs.** The pool must include every
   `coinbase_tx_outputs` entry from `NewTemplate`. This includes non-zero
   outputs such as the development fund, not only `OP_RETURN` outputs.

Operating notes
---------------

- **Default ports.** `sv2-tp` listens on 8442. Keep it bound to 127.0.0.1
  unless the pool or Job Declarator runs on another host.
- **Upgrades.** A new `UPSTREAM_COMMIT` must be re-tested with
  `contrib/sv2-tp/build.sh` and `feature_sv2.py`.
