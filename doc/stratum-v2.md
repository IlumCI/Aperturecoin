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
| Pool, Job Declarator and Translator proxy (SRI `sv2-apps`) | Done for v1 (pool, translator). v2 useful-work pooling done (pool and `aperture-pouw-miner`). v2 through JD is not implemented | `contrib/sri-pool`, `feature_sri_pool.py`, `feature_sri_pool_pouw.py` |
| GPU miner | Done: CUDA/HIP kernels for v2. Not yet run on a GPU | `contrib/gpu-miner` |

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

SRI pool fork
-------------

`contrib/sri-pool` holds the ApertureCoin fork of the SRI applications
(`stratum-mining/sv2-apps`: pool, JD server, JD client, translator, CPU mining
device). It is a patch series against pinned upstream commits
(`UPSTREAM_COMMITS`). `build.sh` fetches those commits, applies the patches
and builds the binaries. The Stratum V2 wire protocol is unchanged, because
its messages carry only header fields. The fork changes four things:

1. **Share and block validation.** ApertureMatMul v1 replaces SHA256d in
   `channels_sv2` (standard and extended channels, server and client), in the
   translator's SV1 share check and in the CPU mining device. The fork adds
   `aperture-sv1-miner`, a multithreaded Stratum V1 CPU miner, as the SV1
   client for the translator. It comes from
   the `aperture_pow` crate, a bit-exact port of `src/crypto/matmulpow.cpp`
   that is tested against `src/test/matmulpow_tests.cpp`'s vectors. The
   block *identifier* reported in `BlockFound` stays SHA256d. `channels_sv2`
   is patched through Cargo's `[patch]` table, so the other upstream protocol
   crates are used unmodified.
2. **Matrix dimension.** The pool has the config key `aperture_pow_dim`
   (512 main/test, 32 regtest). Every role also reads `APERTURE_POW_DIM` or
   `APERTURE_NETWORK`.
3. **Addresses.** `addr()` descriptors and payout identities
   (`sri/solo/<address>/...`, `sri/donate/<pct>/<address>/...`) accept
   ApertureCoin addresses: `sci1`/`tsci1`/`rsci1` and base58 versions
   23/83/111/196/58.
4. **Required coinbase outputs.** No change is needed. The pool appends
   every template output to the coinbase, including the development fund.

Difficulty needs no chain-specific code. Share targets come from the
channel's nominal hashrate, and the block target comes from the template's
`nBits`.

`test/functional/feature_sri_pool.py` runs apertured (regtest, dev fund
enforced), sv2-tp, the SRI pool and the SRI mining device. It checks three
things:

- the pooled blocks are accepted, pay the pool's `rsci1` address and keep the
  dev-fund output;
- every accepted share is the ApertureMatMul hash of its block header,
  compared against the Python reference;
- that hash meets a 16-bit share target, which SHA256d of the header does not
  meet.

With `SRI_TRANSLATOR` and `APERTURE_SV1_MINER` set, a second phase mines
`aperture-sv1-miner` -> translator -> pool, and checks three things:

- the pooled blocks are accepted, with the same checks as above;
- every accepted SV1 share header hashes to the reported ApertureMatMul value;
- some accepted shares miss the share target under SHA256d, so the
  translator validates ApertureMatMul. In test runs, every accepted share missed at a
  share target of about 2^-11 of the hash space.

```sh
contrib/sri-pool/build.sh
SV2TP=contrib/sv2-tp/work/build/bin/sv2-tp \
SRI_POOL=contrib/sri-pool/work/sv2-apps/target/release/pool_sv2 \
SRI_MINING_DEVICE=contrib/sri-pool/work/sv2-apps/target/release/mining_device \
SRI_TRANSLATOR=contrib/sri-pool/work/sv2-apps/target/release/translator_sv2 \
APERTURE_SV1_MINER=contrib/sri-pool/work/sv2-apps/target/release/aperture-sv1-miner \
    test/functional/feature_sri_pool.py
```

### Pooled useful work (ApertureMatMul v2)

The fork pools v2 over the same extension `0x4150`:

| Message | Direction | Content |
|---|---|---|
| UsefulWorkTemplate (0x01) | sv2-tp → pool | template_id, batch_root, model_id, rank, requests |
| SetUsefulWork (0x03, channel message) | pool → miner | channel_id, job_id, batch_root, model_id, rank, requests. Sent before each v2 NewMiningJob |
| SubmitSharesStandard + TLV (0x4150, field 0x01) | miner → pool | op, tile_i, tile_j, span_s, r×r activation panel |
| SubmitUsefulWorkSolution (0x02) | pool → sv2-tp | version, time, nonce, ticket, panel, coinbase |

- **Negotiation.** The miner negotiates `0x4150` with RequestExtensions. The
  pool supports it whenever a protocol model is configured:
  `aperture_protocol_model = "<file.apm>"`, or `aperture_tiny_model = 1` on
  regtest.
- **Template ordering.** The pool holds a v2 NewTemplate until its
  UsefulWorkTemplate arrives, then runs the protocol model over the batch.
  Only then does it release the template, so a miner never gets a v2 job
  without its batch.
- **Share validation.** A v2 job's share is judged by its ticket hash.
  - `channels_sv2::validate_share_with_ticket` calls a verifier registered by
    the pool, and the rest of the share path is unchanged upstream logic:
    accounting, duplicate detection, vardiff and BlockFound.
  - The verifier computes σ from the share's header and the batch_root, then
    `TicketPoW` from the node's C++. These are compiled into the pool from
    this repository (`APERTURE_SRC`) by the `aperture_pouw` crate, not
    reimplemented.
- **Panel verification.** With `aperture_verify_panels` (the default), the
  panel must also equal the pool's own forward-pass activations. Consensus
  does not require this, but the pool rule makes every pooled share work on
  the real inference.
- **Block submission.** A share that meets the network target goes to sv2-tp
  as SubmitUsefulWorkSolution. sv2-tp builds the v2 block, and the embedding
  results are already template coinbase outputs.
- **Miner.** `aperture-pouw-miner` is a standard-channel miner. It runs the
  forward pass per batch and searches all tickets of every nonce with the
  node's kernels (AVX-512 VNNI where available).
- **Test.** `feature_sri_pool_pouw.py` runs apertured (v2 active, dev fund
  enforced), sv2-tp, the pool and `aperture-pouw-miner`. It checks five
  things:
  - the pooled blocks are v2 and accepted;
  - they pay the pool and keep the dev fund;
  - they serve the embedding requests bit-identically to the node's model;
  - their tickets are shares the pool accepted;
  - a miner submitting panels other than the forward pass is rejected
    (`useful-work-panel-mismatch`) before it can find a block.

```sh
SV2TP=contrib/sv2-tp/work/build/bin/sv2-tp \
SRI_POOL=contrib/sri-pool/work/sv2-apps/target/release/pool_sv2 \
APERTURE_POUW_MINER=contrib/sri-pool/work/sv2-apps/target/release/aperture-pouw-miner \
    test/functional/feature_sri_pool_pouw.py
```

Not covered:

- **SV1 translator.** v2 does not go through it, because SV1 has no field
  for the panel.
- **Job Declaration.** v2 is not implemented there: the JDS would have to run
  the forward pass of each declared batch.

Operating notes
---------------

- **Default ports.** `sv2-tp` listens on 8442. Keep it bound to 127.0.0.1
  unless the pool or Job Declarator runs on another host.
- **Upgrades.** A new `UPSTREAM_COMMIT` must be re-tested with
  `contrib/sv2-tp/build.sh` and `feature_sv2.py`.
