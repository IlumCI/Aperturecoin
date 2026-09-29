Agent mandates
==============

A mandate delegates bounded spending authority to an AI agent's key. The
bounds are consensus rules: every node enforces them, whatever the agent
decides or is manipulated into attempting. This follows the
*authority–inference separation* principle (arXiv 2608.30519): model output
never carries spending authority by itself; a deterministic policy does.

A mandate provides:

| Property | How it is enforced |
|---|---|
| Per-period budget | The remaining budget lives in the vault's mutable NFT commitment. Each spend must write back `remaining − amount ≥ 0`. |
| Period rollover | A new period (full budget) needs `period_start + period ≤ new_start ≤ nLockTime`. `OP_CHECKLOCKTIMEVERIFY` proves `new_start` has been reached. |
| Expiry | No new period may start at or after `expiry`. After that, only the rest of the final period's budget can be spent. |
| Allowlist | One tapleaf per allowed destination. The Taproot tree is the allowlist, and each leaf pins `OP_OUTPUTBYTECODE 0`. |
| Vault continuity | Output 1 must be the same script and the same mandate NFT (mutable), with value ≥ input − amount − `max_fee`. |
| Owner control | The owner key is the Taproot internal key and can sweep, top up or revoke at any time through the key path. |

Consensus cannot enforce "spend before height H", because a UTXO script cannot
see the current height and transactions have no expiry. Expiry is therefore
defined as "no budget refresh after H". A hard stop is the owner sweeping the
vault.

Construction
------------

The vault is a P2TR output: internal key = owner, one leaf per destination. It
carries a mutable NFT whose category is the mandate ID, created at genesis
(see `doc/tokens.md`). The NFT commitment holds the state:

```
state = [len(remaining) + 1] ‖ remaining ‖ period_start      (script numbers, minimal)
```

The length byte makes the split unambiguous, so an agent cannot reinterpret
the bytes to claim a larger budget.

The leaf script (`contrib/aperture-sdk/aperture_sdk/mandate.py`,
`agent_leaf`) takes this witness stack:

```
sig  new_start  amount  remaining_old  period_start_old
```

and checks, in order:

1. The old state matches `OP_UTXOTOKENCOMMITMENT` of the spent vault.
2. The available budget:
   - if `new_start > period_start_old`, it is a new period: `period_start_old + period ≤ new_start`, `new_start < expiry`, `new_start OP_CHECKLOCKTIMEVERIFY`, available = budget;
   - otherwise `new_start = period_start_old` and available = remaining.
3. `new_remaining = available − amount ≥ 0`.
4. Output 1 carries the new state, and has the same bytecode, mandate category (with its mutable capability byte) and bounded value.
5. Output 0 pays exactly `amount` to this leaf's destination.
6. `agent_pubkey OP_CHECKSIG`. The BIP341 signature commits to the whole transaction.

Mapping to AP2 mandates
-----------------------

| AP2 (Google, FIDO) | ApertureCoin mandate |
|---|---|
| Intent mandate (what the agent may buy, limits) | Budget, period, expiry and allowlist, fixed in the vault script |
| Cart mandate (a specific purchase) | The agent's spend transaction: destination and amount |
| Payment mandate (authorization to pay) | The agent's BIP340 signature, valid only inside the vault's rules |

Using it
--------

```python
from aperture_sdk.mandate import Mandate, xonly

m = Mandate(owner_xonly, agent_xonly, budget=100_000, period=720, expiry=height + 30 * 720,
            allowlist=[merchant_a_spk, merchant_b_spk], max_fee=10_000, category=genesis_txid)

m.script_pubkey                                     # fund m.vault_output(value, budget, height) at genesis
tx, remaining = m.spend(vault, merchant_a_spk, 60_000, fee, remaining=..., period_start=..., new_start=..., locktime=height)
m.sign_agent(tx, spent_output, agent_privkey, merchant_a_spk, remaining=..., period_start=..., new_start=..., amount=60_000)
m.sweep(vault, spent_output, owner_privkey, owner_spk, fee)
```

`test/functional/feature_mandate.py` covers the full life cycle:

- spends within budget;
- over-budget, forged-state, non-allowlisted and early-rollover spends, each rejected by consensus;
- the CLTV rollover;
- the expiry cutoff;
- the owner sweep.
