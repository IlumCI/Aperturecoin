aperture-pay
============

A reference HTTP 402 ("payment required") flow for agents paying for API
results with atomic delivery. See `doc/agent-payments.md`.

The flow itself lives in `contrib/aperture-sdk/aperture_sdk/x402.py`:
`make_quote`, `contract_for` and `recover`, with the Taproot contracts in
`payments.py`. `test/functional/feature_agent_payments.py` runs the full
exchange against a regtest node:

1. quote
2. fund
3. settle (the provider reveals K)
4. the agent decrypts the result

It also covers refund and escrow paths.

The quote is carried in the HTTP 402 response body as JSON:

```json
{"amount": 50000, "payee": "<x-only hex>", "key_hash": "<sha256(K) hex>", "timeout": 144, "ciphertext": "<hex>"}
```

and the agent answers with `{"txid": ..., "vout": ...}` of its funding output.
