ApertureCoin Core
=================

https://github.com/IlumCI/Aperturecoin

ApertureCoin (ticker **SCIENCE**) is an experimental proof-of-work
cryptocurrency. Its proof of work is a matrix multiplication (ApertureMatMul)
built for the int8 matrix engines in current AI accelerators and CPUs.
ApertureCoin Core is forked from Litecoin Core 0.21 (itself derived from
Bitcoin Core) and runs as an independent network with its own genesis block.

Status: pre-launch. Neither mainnet nor testnet is running yet.

Chain parameters
----------------

| Parameter          | Value                                                                    |
|--------------------|--------------------------------------------------------------------------|
| Proof of work      | ApertureMatMul v1 (n = 512), see [doc/matmulpow.md](doc/matmulpow.md)    |
| Block interval     | 120 s                                                                    |
| Difficulty         | ASERT (aserti3-2d), per block, 2-day half-life, anchored at block 1      |
| Block subsidy      | 50 SCIENCE, halving every 1,051,200 blocks (~4 years)                    |
| Supply cap         | ~105,120,000 SCIENCE                                                     |
| Development fund   | 1.5% of the subsidy until the first halving, enforced by consensus       |
| Soft forks         | P2SH, BIP34/65/66, CSV, SegWit, Taproot enforced from genesis            |
| Native tokens      | fungible + NFT (CashTokens semantics) from genesis, see [doc/tokens.md](doc/tokens.md) |
| Agent layer        | covenants, mandates, atomic payments, batch auctions: [covenants](doc/covenants.md), [mandates](doc/agent-mandates.md), [payments](doc/agent-payments.md), [auctions](doc/batch-auctions.md) |
| Roadmap (drafts)   | useful-work PoW v2 (mining runs the protocol embedding model) [doc/pouw-v2.md](doc/pouw-v2.md), [doc/protocol-model.md](doc/protocol-model.md); agent research market [doc/research-market.md](doc/research-market.md) |
| Mining protocol    | Stratum V2 Template Provider + reference miner, see [doc/stratum-v2.md](doc/stratum-v2.md) |
| MWEB               | removed (Bitcoin transaction, block and undo serialization)              |
| Addresses          | `sci1...` (bech32/bech32m), `A...` (P2PKH), `a...` (P2SH)                |
| Default ports      | P2P 9433, RPC 9432; testnet 19435 / 19432; regtest 19544 / 19543         |
| Binaries           | `apertured`, `aperture-cli`, `aperture-tx`, `aperture-wallet`, `aperture-qt` |
| Data directory     | `~/.aperture`, configuration file `aperture.conf`                        |

Design references (arXiv):

- [2504.09971](https://arxiv.org/abs/2504.09971): proofs of useful work from
  matrix multiplication. This is the basis for the planned useful-work
  upgrade (v2).
- [2606.04819](https://arxiv.org/abs/2606.04819): an empirical study of a
  deployed matmul-PoW network doing no useful work. It is the reason v1 claims
  only hardware alignment.
- [2606.06700](https://arxiv.org/abs/2606.06700): the economics of
  proof-of-useful-work.
- [2511.11538](https://arxiv.org/abs/2511.11538): exploits against slow
  retarget windows. This motivated the per-block ASERT adjustment.

Mining
------

- **Regtest:** `generatetoaddress` mines on the CPU.
- **External miners:** use `getblocktemplate` and `submitblock`. When the
  development fund applies, the template includes a `devfund` object
  (`script`, `amount`). The coinbase must pay at least that amount to that
  script. `coinbasevalue` includes the fund.
- **Reference implementation:** `src/crypto/matmulpow.cpp` in C++, and
  `test/functional/test_framework/aperture_matmulpow.py` in Python.

Building
--------

See `doc/build-*.md`. On Linux:

```
./autogen.sh
./configure --with-incompatible-bdb   # C++17 compiler required
make -j$(nproc)
make check
test/functional/test_runner.py
```

License
-------

ApertureCoin Core is released under the terms of the MIT license. See
[COPYING](COPYING) for more information or see
https://opensource.org/licenses/MIT. It keeps the copyright notices of the
Bitcoin Core and Litecoin Core developers.
