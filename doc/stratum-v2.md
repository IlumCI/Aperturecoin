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
| ApertureMatMul v2 over SV2 (useful-work extension) | Done: TP, miner, end-to-end test | `patches/0002-*`, `feature_sv2_pouw.py` |
| End-to-end test | Done. Skipped unless `SV2TP`/`SV2MINER` are set | `test/functional/feature_sv2.py` |
| Pool, Job Declarator and Translator proxy (SRI `sv2-apps`) | Not started | Planned fork |
| GPU miner | Not started | Planned |

ApertureMatMul v2: the useful-work extension
--------------------------------------------

A v2 header carries a batch_root, a ticket and the ticket's activation panel
(`doc/pouw-v2.md`), and the miner needs the block's embedding requests to run
the protocol model. The standard Template Distribution messages carry neither.
ApertureCoin defines them as an SV2 **extension**:
- `extension_type = 0x4150` ("AP") in the frame header.
- Message types are scoped to the extension, so they cannot collide with
  standard messages.
- Clients that do not implement the extension never receive or send these
  messages for v1 templates.

| Type | Direction | Payload (little-endian) |
|---|---|---|
| `0x01` UsefulWorkTemplate | TP → client, right after `NewTemplate` of a v2 template | `template_id:u64` `batch_root[32]` `model_id[32]` `rank:u8` `request_count:u16` then per request `token_count:u16` `token_id:u24…` |
| `0x02` SubmitUsefulWorkSolution | client → TP, instead of `SubmitSolution` | `template_id:u64` `version:u32` `timestamp:u32` `nonce:u32` `op:u16` `tile_i:u16` `tile_j:u16` `span_s:u16` `panel_len:u16` `panel[panel_len]` `coinbase_len:u32` `coinbase` (with witness) |

The flow:
1. **Node.** `apertured` runs the forward pass while it builds the template.
   Its coinbase results (`OP_RETURN "APEM"…`) are **mandatory coinbase
   outputs** in `NewTemplate`, like the development fund. `getblocktemplate`
   exposes the batch as `powv2 {batch_root, model_id, rank, requests,
   results}`.
2. **Miner.** It loads the protocol model itself, with `-protocolmodel=<file.apm>`
   or `-tinymodel` on regtest, and checks its model id against the template.
   It runs the forward pass over the requests (the useful work) and searches
   tickets over its own activations with the pinned weights. On a win it
   sends `SubmitUsefulWorkSolution`.
3. **Template Provider.** `sv2-tp` rebuilds the block and inserts the v2
   header extension after the 80 v1 bytes, then calls `submitblock`.

Upstream changes (`patches/0002-aperture-useful-work-extension.patch`):
- `extension_type` plumbing in `Sv2NetHeader`/`Sv2NetMsg`/transport. The
  internal wrapping format is unchanged, so the upstream `test_sv2` suite
  passes.
- An `ExtensionMessage` hook in `Sv2Connman`.
- `BlockTemplate::apertureUsefulWork()` and `apertureSubmitUsefulWork()`,
  whose defaults are no-ops.
- Sending and receiving in `Sv2TemplateProvider`.

The template provider only forwards opaque payloads. All ApertureCoin
encoding lives in `overlay/src/aperture/rpc_mining.cpp`.

**Pools.** An SRI pool or Job Declarator must forward the extension payloads
to its downstream miners, and must validate shares as v2 tickets, which needs
the protocol-model weights. The work described under "Porting the SRI pool"
applies unchanged, plus this extension.

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
