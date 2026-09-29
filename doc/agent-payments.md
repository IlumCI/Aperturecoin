Agent payments
==============

These are two payment primitives for agents buying services from other agents
or from APIs. Both are Taproot outputs whose internal key is the provably
unspendable BIP341 point H, so only their script paths exist. Neither needs a
new consensus rule; the escrow's capture leaf uses the tapscript
introspection opcodes (`doc/covenants.md`).

Atomic pay-for-result
---------------------

x402-style flows (HTTP 402, then pay, then retry) are not atomic. The agent
can pay and receive nothing. Studies of x402 (arXiv 2605.11781, 2607.19545,
2603.01179) also show that facilitators concentrate trust. Here the payment
itself delivers the result:

```
settle: OP_SHA256 <sha256(K)> OP_EQUALVERIFY <payee> OP_CHECKSIG
refund: <timeout> OP_CHECKSEQUENCEVERIFY OP_DROP <payer> OP_CHECKSIG
```

1. The provider computes the result, encrypts it under a fresh key K, and
   answers **402** with `{amount, payee, key_hash = sha256(K), timeout, ciphertext}`.
2. The agent funds the contract for `amount` and sends back the outpoint.
3. The provider claims the payment. Claiming requires publishing K in the
   witness.
4. The agent reads K from the chain and decrypts the result.

The provider cannot be paid without handing over K, and the agent cannot
obtain K without the provider being paid. If the provider never settles, the
agent refunds after `timeout` blocks.

The envelope cipher in the SDK (SHA-256 counter mode) is for reference only.
Production systems should use an authenticated cipher keyed by K.

Refundable escrow (authorize, then capture)
-------------------------------------------

Instant, final payments do not suit most commerce (arXiv 2609.02208). The
escrow authorizes an amount that the merchant may capture in part:

```
capture: OP_1 OP_OUTPUTBYTECODE <payer_spk> OP_EQUALVERIFY
         OP_0 OP_OUTPUTVALUE OP_1 OP_OUTPUTVALUE OP_ADD
         OP_INPUTINDEX OP_UTXOVALUE <fee_cap> OP_SUB OP_GREATERTHANOREQUAL OP_VERIFY
         <merchant> OP_CHECKSIG
refund:  <timeout> OP_CHECKSEQUENCEVERIFY OP_DROP <payer> OP_CHECKSIG
```

- **Capture.** The merchant takes output 0 and must return the uncaptured
  remainder to the payer in output 1. It may not skim more than `fee_cap` as
  fee.
- **Refund.** After `timeout` blocks the payer can reclaim an uncaptured
  authorization.

Code and tests
--------------

- `contrib/aperture-sdk/aperture_sdk/payments.py`: the `ServicePayment` and
  `Escrow` contracts and their signing.
- `contrib/aperture-sdk/aperture_sdk/x402.py`: the quote, contract and
  recovery steps of the HTTP 402 flow (`contrib/aperture-pay/README.md`).
- `test/functional/feature_agent_payments.py` covers:
  - quote round trip, funding, and rejection of settlement with a wrong key;
  - rejection of an early refund, then settlement with K and recovery of the
    result from the chain;
  - refund after the timeout;
  - escrow: capture that does not return the remainder, capture that exceeds
    the fee cap, a valid partial capture, and refund after the timeout.

Mandates (`doc/agent-mandates.md`) and these payments compose: an agent can
fund service payments from a mandate vault, within its budget.
