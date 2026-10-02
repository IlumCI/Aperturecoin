Mainnet launch checklist
========================

A fair launch means that nobody has an information or timing advantage at
genesis. It also means that anyone can check this independently. Every item
below is a gate: mainnet is not announced until all of them are complete.

0. Viability gate (decided)
---------------------------

Mainnet is viable when the public testnet shows that the useful-work loop
works with people outside the project. All of these, measured over the
testnet soak:

- [ ] **Miners:** at least 5 independent mining operators (not the
      project), with at least one CPU and one GPU miner.
- [ ] **Demand:** at least 2,000 paid embedding requests served, from at least
      3 independent payers (wallets the project does not control).
- [ ] **Verification:** at least 2 independent full nodes have recomputed
      every served result (`checkblockembeddings` over the whole soak) with
      zero mismatches.
- [ ] **Security assumption:** the bounty on assumption A1 and Fold
      (`doc/pouw-v2.md`) has been open for at least 4 weeks without a break.
- [ ] **GPU:** `gpu_selftest` passes on at least one NVIDIA and one AMD GPU.
- [ ] **Transparency:** the useful share (`getusefulshare 2016`) is published
      weekly from the first testnet week.

There is deliberately no minimum useful share: it is bounded by paid demand
per block, not by miner behaviour (`doc/pouw-v2.md`, "Usefulness
accounting"). It is published, not gated.

With a public testnet running, these gates can be met in about 5 to 6
weeks; the soak below is the long pole.

1. Testnet soak
---------------

- [ ] The public testnet has run for at least 4 weeks, with at least 5
      independent miners and both CPU and GPU miners represented.
- [ ] ASERT convergence is confirmed: the mean block interval over the last
      2,000 blocks is within 120 s ± 5%.
- [ ] No consensus failures, no crashes, and no unexplained reorgs deeper than
      2 blocks.
- [ ] The functional and unit test suites pass on the release tag, and CI is
      green (`.github/workflows/ci.yml`).

2. Parameters frozen
--------------------

- [ ] Initial difficulty (`powLimit` in `CMainParams`) is set from measured
      testnet hash rate. Block 1 is mined at `powLimit` and anchors ASERT, so
      if `powLimit` is too easy, the first blocks arrive faster than the
      120 s target until ASERT catches up. Aim for block 1 taking at least
      60 s at the expected launch hash rate.
- [ ] The development fund script is set from the key ceremony
      (`doc/devfund-key-ceremony.md`). The unspendable placeholder is gone.
- [ ] No premine and no special allocations. Genesis pays only the unspendable
      `OP_RETURN "SCIENCE"`.

3. Genesis
----------

- [ ] The genesis timestamp is announced at least 7 days in advance, together
      with the release tag.
- [ ] The genesis timestamp message references something that proves it was
      not created earlier, for example a recent block hash of another chain or
      a same-day headline. The message is at most 87 bytes, because the
      coinbase scriptSig must stay at or below 100 bytes.
- [ ] The genesis block is re-mined with `contrib/genesis/mine_genesis.py`, and
      the asserts in `CMainParams` are updated.
- [ ] Anyone can reproduce the genesis hash from the published parameters with
      `contrib/genesis/mine_genesis.py`.

4. Binaries
-----------

- [ ] The release tag is signed by the maintainer. The tag's commit passes
      `contrib/verify-commits/verify-commits.py`.
- [ ] Guix builds (`contrib/guix/guix-build.sh`) come from at least 2
      independent builders, with matching `SHA256SUMS` published in
      `IlumCI/aperture-guix.sigs`.
- [ ] Source and binaries are published at the same moment. There is no
      early-access distribution.
- [ ] Miners can use the reference CPU miner, the GPU miner and the Stratum V2
      pool software on release day.

5. Network bootstrap
--------------------

- [ ] At least 3 independent DNS seed operators are live, on at least 2 hosting
      providers or ASNs (`doc/seeder-runbook.md`).
- [ ] Fixed seeds are generated from at least 2 operators' crawls.
- [ ] Bootstrap `-addnode` peers are posted in the release announcement.

6. After launch
---------------

- [ ] Publish block 1's hash and time.
- [ ] Monitor the block interval, orphan rate and mining pool distribution.
      Publish a report after the first 7 days.
- [ ] Update `nMinimumChainWork` and `defaultAssumeValid` in the first point
      release.
