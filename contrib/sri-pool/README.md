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
  - the `[patch]` that swaps in the patched `channels_sv2`;
  - config examples and `aperture/README.md`.
- `patches/0002-channels-sv2-aperture.patch`: applied to a copy of
  `sv2/channels-sv2` from stratum-mining/stratum. Share and block validation
  use the ApertureMatMul hash, and `BlockFound` keeps the SHA256d block id.

```sh
contrib/sri-pool/build.sh            # -> contrib/sri-pool/work/sv2-apps/target/release/
```

Design, configuration and the end-to-end test: `doc/stratum-v2.md`, section
"SRI pool fork".
