# SRI pool fork for ApertureCoin

This directory holds the ApertureCoin fork of the Stratum V2 reference
applications ([stratum-mining/sv2-apps](https://github.com/stratum-mining/sv2-apps)):
pool, JD server, JD client, translator and CPU mining device. The fork is a
patch series against the pinned upstream commits in `UPSTREAM_COMMITS`:

- `patches/0001-sv2-apps-aperture.patch`:
  - the `aperture_pow` crate (ApertureMatMul v1);
  - ApertureCoin addresses in `addr()` and payout identities;
  - the pool's `aperture_pow_dim` key;
  - translator and mining-device hashing;
  - `aperture-sv1-miner`, the Stratum V1 CPU miner used with the translator;
  - ApertureMatMul v2 pooling (extension `0x4150`):
    - the `aperture_pouw` bindings to this repository's consensus C++;
    - the pool's forward pass per template, ticket shares and panel
      verification;
    - `aperture-pouw-miner`;
  - the `[patch]` that swaps in the patched `channels_sv2`;
  - pool and translator config examples, and `aperture/README.md`.
- `patches/0002-channels-sv2-aperture.patch`: applied to a copy of
  `sv2/channels-sv2` from stratum-mining/stratum. Share and block validation
  use the ApertureMatMul hash, and `BlockFound` keeps the SHA256d block id.

```sh
contrib/sri-pool/build.sh            # -> contrib/sri-pool/work/sv2-apps/target/release/
```

`build.sh` sets `APERTURE_SRC` to this checkout. The pool and
`aperture-pouw-miner` compile the node's ApertureMatMul v2 and protocol-model
sources from it.

Design, configuration and the end-to-end test: `doc/stratum-v2.md`, section
"SRI pool fork".
