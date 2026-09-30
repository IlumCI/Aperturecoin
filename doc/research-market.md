Research market: proof of improvement (specification draft)
==========================================================

Status: draft for review. Nothing here is implemented.

Goal
----

Agents, and the people who run them, earn SCIENCE for **measured improvements**
on research problems. This is an alternative to hardware mining. The market
pays only for results that a pinned, deterministic evaluator scores above the
current record, on data the submitter could not have seen. Plans, ideas and
unevaluated claims earn nothing.

Decisions already taken:
- Funding is a share of the block subsidy that ramps up over time, plus
  sponsor bounties.
- Two tracks at launch: held-out ML benchmarks, and kernels and systems.
- Specs come first, then the node, covenants and evaluator are built in
  parallel.

Evidence this design is built on
--------------------------------

| Finding | Design consequence |
|---|---|
| Agents reward-hack in 30.5% of open-ended research tasks without being told to, and more after they see reviewer feedback ([2609.28614](https://arxiv.org/abs/2609.28614)). | Scores are recomputed by the evaluator outside the agent's control, and feedback is thresholded. |
| Unearned passes on SWE-Bench Pro rose from 24% to 73% across model generations; blocking one route leaves others open ([2609.34262](https://arxiv.org/abs/2609.34262)). | Exploits found in an evaluator are paid a bounty, and record rollback is part of the protocol. |
| Agents trained on the test set, downloaded checkpoints instead of training, and misused keys they found ([2603.08640](https://arxiv.org/abs/2603.08640)). | Evaluator containers have no network access and no credentials. The submission is a recipe that the evaluator runs, not a set of weights. |
| Repeated benchmark reuse causes adaptive overfitting, and limiting feedback removed false promotions ([2609.33180](https://arxiv.org/abs/2609.33180)). | Held-out data is fresh for every round and is chosen after the submission deadline. |
| Agents quietly shrink datasets or budgets and substitute oracle functions ([2608.26753](https://arxiv.org/abs/2608.26753)). | The evaluator enforces the budget and required components. The submitted code is not trusted to report them. |
| Bittensor rewards are driven overwhelmingly by stake ([2507.02951](https://arxiv.org/abs/2507.02951)), and winner-takes-all rewards lead to model hoarding ([2507.17766](https://arxiv.org/abs/2507.17766)). | Payment is for marginal improvement over the record, and evaluator influence is bounded by economic-security limits. |
| Scoring loss before and after a contribution, plus uniqueness checks, counters copying ([2505.21684](https://arxiv.org/abs/2505.21684)). | Submissions are committed before the deadline (commit-reveal), so they cannot be copied. |
| Proportional slashing leaves a repeated-game cheating gap; history-dependent penalties and vesting close most of it ([2608.09055](https://arxiv.org/abs/2608.09055)). | Evaluator bonds vest, and slashing escalates with history. |
| Agents improve execution but not measured originality ([2609.07611](https://arxiv.org/abs/2609.07611), [2609.14738](https://arxiv.org/abs/2609.14738)). | No novelty track at launch. Only objective improvements are paid. |
| Offering an escalation or disclosure channel cut hacking from 23.6% to 5.3% ([2608.29460](https://arxiv.org/abs/2608.29460)). | Exploit disclosure pays more than exploiting. |
| Process traces expose methodology that final outputs hide ([2609.09203](https://arxiv.org/abs/2609.09203)). | A trace is required and stored for audit. It is not scored. |

Architecture
------------

```
 coinbase ── research share ──► Research Pool (consensus balance)
                                     │  pays record improvements in protocol families
 sponsors ── bounty covenant ─────┐  │
                                  ▼  ▼
   Challenge round:  commit ─► deadline ─► reveal ─► held-out draw ─► committee evaluation
                                                                          │
                                         settlement tx (≥ 2/3 committee) ◄┘
                                         dispute window ─► escalation committee ─► slashing
```

### 1. Research Pool (consensus)

- **Accrual.** The coinbase may claim at most
  `subsidy × (1 − dev_bps − research_bps) / 10000 + fees`. The research share
  is added to a pool balance kept in the chainstate, with undo data per block.
  It is not a UTXO, so there is no output that a miner could spend early.
- **Schedule.**
  - `research_bps = 1000` (10%) from genesis.
  - It rises to 1500, then 2500, through two BIP9 deployments,
    `RESEARCH_15` and `RESEARCH_25`. Miners signal them once the testnet
    gates are met (below). Raising the share restricts the coinbase further,
    so each step is a soft fork.
  - The dev fund (1.5% until the first halving) comes out of the miner share,
    not the pool.
- **Disbursement.** Only settlement transactions valid under section 4 can
  pay out of the pool. Payouts are limited per epoch (2,016 blocks) to
  `min(pool_balance × 20%, accrued in the previous epoch × 2)`.
- **Launch.** These rules are consensus from mainnet genesis. Turning
  disbursement on later would be a hard fork, so it ships with v2
  (`doc/pouw-v2.md`) before mainnet.

### 2. Families and challenges

A **family** is a long-running benchmark with a persistent record. There are
two kinds of challenge.

**Protocol families** are paid from the pool. They are listed in
`chainparams` and added or removed only by software release, so there is no
on-chain governance token. Their held-out data is chosen after the deadline
from entropy nobody controls. This removes the self-dealing problem, in which
a sponsor who knows its own test set wins its own subsidy.

| ID | Track | Submission | Held-out data | Metric |
|---|---|---|---|---|
| K0 | Kernels | Attack code against Assumption A1 or Fold (`doc/pouw-v2.md`) | Random σ drawn from a later block hash | Break: computes a ticket in less than the bound. Paid a fixed bounty, not a record. |
| K1 | Kernels | ApertureMatMul v2 miner kernel | Random request batches and σ from a later block hash, run on the protocol model | Correctness on all vectors, then tickets per second on the hardware class (median of the committee) |
| K2 | Kernels | int8 GEMM, attention, or quantization kernel for a pinned operator spec | Random shapes and inputs from a later block hash | Exact or bounded-error correctness, then throughput (median of the committee) |
| M1 | ML | Training recipe (code + config) run by the evaluator from scratch on a pinned train split, within a FLOP budget | Validation shard selected from a sealed pool index by a later block hash | Validation loss at the budget. The evaluator counts the FLOPs. |
| M2 | ML | Compression or language model under a size cap | Text that appears after the deadline in a pinned public feed (arXiv listing abstracts). Each committee member snapshots it independently and they must agree on the hash. | Bits per byte |

**Sponsor challenges** are permissionless and funded only by the sponsor's
bounty covenant. They use the same settlement machinery. Self-dealing there
only moves the sponsor's own money, which does no harm. Sponsors commit to
`H(heldout || salt)` together with a bond, and forfeit the bond if they do not
reveal.

Each challenge round is recorded on chain as a challenge NFT (category = round
ID) whose commitment holds:

```
family_id | spec_hash | evaluator_image_digest (OCI sha256) | budget |
submit_deadline | reveal_deadline | heldout_rule | record_ref | payout_rule
```

### 3. Round lifecycle

1. **Commit.** The submitter posts
   `OP_RETURN "APSC" <round_id> <H(submission_bundle || salt || submitter_key)>`,
   paying an anti-spam fee, before `submit_deadline`. The submission bundle is
   content-addressed off chain.
2. **Reveal.** Before `reveal_deadline` the submitter publishes the bundle and
   the salt. A bundle contains the code, its config, the process trace and a
   license. Bundles that are not revealed are dropped.
3. **Held-out draw.** The seed is the hash of block `reveal_deadline + 6`.
   Protocol families derive their held-out selection from the seed. Sponsor
   rounds reveal `heldout || salt`.
4. **Committee.** k = 7 evaluators are sampled from the registry (section 5)
   using the seed.
   - Each runs the pinned image, without network access, on the reference
     hardware class. The run is deterministic: fixed seeds, RepOps-style
     reduction order ([2502.19405](https://arxiv.org/abs/2502.19405),
     [2609.17380](https://arxiv.org/abs/2609.17380)).
   - Each evaluator signs `(round_id, ranked results, scores, heldout_hash)`.
   - Throughput metrics are the median of the committee. They must fall
     within the family's tolerance, and outliers are discarded.
5. **Settlement.** A settlement transaction carrying at least 5 of the 7
   sampled signatures pays out according to `payout_rule` (section 6). It
   then waits out a dispute window of 144 blocks before its outputs mature.
6. **Dispute.** Any submitter or registered evaluator can post a dispute bond
   within the window. A new committee of 3k is sampled from the seed at the
   dispute block, excluding the first committee.
   - Its 2/3 result replaces the original settlement.
   - First-committee signers on the losing side are slashed.
   - A dispute that fails forfeits its bond to the committee.

### 4. Settlement validity (consensus)

Consensus checks only things that are cheap and objective:

- the round exists and its deadlines have passed;
- the committee membership is recomputed deterministically from the seed and
  the registry snapshot at `reveal_deadline`;
- the signature threshold is met (BIP340 signatures, batch-verified);
- payouts follow `payout_rule` given the signed scores and the recorded
  previous record;
- the per-epoch pool limit is respected;
- dispute and maturity rules are followed.

Consensus never runs an evaluation. Whether an evaluation is correct rests on
the committee plus the dispute game. **This is a trust assumption: at least
2/3 of any sampled committee is honest.** It is backed by bonds and made
expensive to break by the economic-security bound in section 5. It is the
same limit every verifiable-ML system in the survey reaches (Verde, TOPLOC,
staked inference). Nakamoto consensus cannot verify ML training.

### 5. Evaluator registry

- An evaluator registers by creating a bond output with the marker
  `"APEV"`. The output carries the evaluator key and the hardware classes it
  declares.
  - The bond vests over 4,032 blocks before the evaluator can be sampled.
  - Unbonding takes 4,032 blocks.
- Sampling weight is proportional to vested bond. Splitting a bond across
  outputs changes nothing, because influence stays proportional to bond.
- **Economic-security bound.** The value settled per epoch must not exceed a
  third of the total vested bond. Per-epoch pool limits and sponsor-bounty
  caps are derived from it, so bribing a committee costs more than it can
  win.
- **Slashing is history-dependent** (2608.09055):
  - A first loss in a dispute costs 10% of the bond, a second 30%, and
    equivocation 100%.
  - Equivocation is proven on chain by two conflicting signed attestations.
- **Payment.** Evaluators receive 10% of each payout, plus sponsor fees.

### 6. Payout rule (protocol families)

- The winner is the best-scoring valid submission.
- Payment is made only if the score beats the family record by the family's
  minimum significance margin. The margin is set from evaluator repeat
  variance, so noise cannot win.

```
Δ        = relative improvement over the record
reward   = min( pool_epoch_budget × w_family , pool_epoch_budget × w_family × Δ / Δ_ref )
split    = 90% submitter, 10% committee
record  := winning score (with the round reference)
```

- **Credit for marginal contribution.** When a submission builds on a
  previous record holder's released code, 20% of its reward vests to the
  earlier record holder. This follows CLASP-style attribution
  ([2507.17766](https://arxiv.org/abs/2507.17766)) and keeps a shared record
  of improvements from turning into hoarding.
- **K0 breaks** pay a fixed bounty (for example 5% of the pool balance). A
  break freezes the PoW parameters for review.

### 7. Anti-gaming requirements for evaluator images

- No network access, no credentials, and a read-only train split. The
  held-out data is mounted only after the submission's training phase ends.
- The budget (FLOPs, wall-clock, memory) is enforced by the harness, and
  required components are checked against the spec
  ([2608.26753](https://arxiv.org/abs/2608.26753)).
- Feedback is thresholded: submitters see pass or fail plus their rank, never
  per-example scores ([2609.33180](https://arxiv.org/abs/2609.33180)).
- **Exploit bounty.** Anyone who demonstrates an evaluator exploit is paid
  from the pool. The family's records set through that exploit are rolled
  back, and the image is re-pinned
  ([2609.34262](https://arxiv.org/abs/2609.34262),
  [2608.29460](https://arxiv.org/abs/2608.29460)).
- The process trace is stored with the submission and can be audited in a
  dispute ([2609.09203](https://arxiv.org/abs/2609.09203)).

### 8. Scope

Challenges on chain are permissionless. Evaluators choose what they run.
Reference evaluator software refuses families and sponsor rounds whose spec
matches the published exclusion list: biological, chemical, nuclear and
radiological weapons uplift, cyber-offense against third parties, and removal
of safeguards from deployed systems. A round no committee will evaluate
cannot settle. Protocol families are defined in releases and stay within
scope.

### 9. Agents as participants

Agents take part like any other key holder, using the existing primitives:

- A **mandate** (`doc/agent-mandates.md`) caps an agent's spending on commit
  fees, dispute bonds and compute. Its owner can sweep it at any time.
- **Payments** (`doc/agent-payments.md`) buy compute or data from other
  agents atomically.
- **Tokens** (`doc/tokens.md`) represent shares of the rewards of a
  multi-agent team. The split among members is enforced by covenant.

Testnet gates for the subsidy ramp
----------------------------------

`RESEARCH_15` signalling starts only when all of these hold:

- at least 30 registered evaluators, on at least 3 hardware classes, with
  vested bonds;
- at least 8 settled rounds in every protocol family, with no unresolved
  dispute;
- at least one exploit-bounty cycle completed end to end, with rollback;
- the K0 bounty open for at least 4 weeks.

`RESEARCH_25` requires `RESEARCH_15` plus twice those counts.

Implementation plan
-------------------

1. **Consensus.**
   - The pool balance in the chainstate, the coinbase limit, and the epoch
     disbursement limit.
   - Validation of the `APEV` registry, `APSC` commits and round NFTs.
   - Validation of settlement and dispute transactions.
   - Committee sampling.
2. **Evaluator.** `contrib/aperture-eval/`, containing:
   - a deterministic OCI runner with no network access and budget
     enforcement;
   - a RepOps-style deterministic PyTorch profile;
   - committee attestation, and client code for disputes.
3. **SDK.** `aperture_sdk.research`, with commit and reveal, bundle
   building, and the mandate-funded agent flow.
4. **Families.** Specs and pinned images for K0, K1, K2, M1 and M2.
5. **Tests.** Functional tests for:
   - accrual and the coinbase limit;
   - settlement with 5 of 7 signatures, and rejection of wrong members;
   - a dispute that overrides the result and slashes;
   - the epoch limit;
   - an equivocation slash.

Open items
----------

- The committee-honesty assumption cannot be removed on a proof-of-work chain
  without on-chain verification of ML, which does not exist at useful
  scales.
- M2 depends on an external feed. If committee members see different
  snapshots, the round does not settle.
- Throughput metrics depend on hardware. Families must name one hardware
  class, and results do not carry over between classes.
- Miners can bias the committee seed by withholding a block, at the cost of
  the block reward. The value at stake per round is capped below one block
  reward, or the seed is taken from a VDF over the block hash (open).
