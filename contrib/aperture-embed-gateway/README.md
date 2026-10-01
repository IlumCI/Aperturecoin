aperture-embed-gateway
======================

An OpenAI-compatible embeddings endpoint whose results are computed by
ApertureCoin's useful-work mining (`doc/pouw-v2.md`). Any OpenAI client
library works unchanged:

```python
from openai import OpenAI
client = OpenAI(base_url="http://127.0.0.1:8402/v1", api_key="unused")
r = client.embeddings.create(model="aperture", input=["first passage", "second passage"])
r.data[0].embedding                 # unit-length float vector (1024 dims for the testnet model)
r.data[0].model_extra["aperture"]   # model_id, int8 result, on-chain request outpoint, status
```

How a request is served
-----------------------

1. The gateway computes the embedding with the node's protocol model and
   returns it at once. The integer inference profile is bit-exact, so this
   is the same value the chain will record.
2. It settles the input as an on-chain embedding request, paid from the
   node's wallet. The miner of the block that includes the request must
   include the result. Full nodes recompute it. On optimistic chains
   (testnet), anyone can prove a wrong result with a fraud claim, which takes
   the miner's coinbase.
3. `GET /v1/requests/<txid>:<vout>` reports the request as `pending` or
   `confirmed` (block hash, height, on-chain result).

Request options go in an `aperture` object next to the OpenAI fields:

| Option | Default | Meaning |
|---|---|---|
| `settle` | `true` | Create the on-chain request. `false` computes locally only, with no fee. |
| `wait` | `0` | Seconds to hold the response until the request is mined. The response then carries `blockhash`, `height` and `matches_local`. |

`encoding_format` may be `float` (default) or `base64` (little-endian
float32), as in the OpenAI API.

Running it
----------

```
contrib/aperture-model/fetch_protocol_model.py          # model + tokenizer
apertured -testnet -wallet=gw
contrib/aperture-embed-gateway/gateway.py \
    --rpcurl=http://127.0.0.1:19432/wallet/gw \
    --rpccookiefile=<datadir>/<testnet dir>/.cookie \
    --tokenizer=<datadir>/models/src/tokenizer.json
```

Retrieval queries should carry the model's instruction prefix, for example
`"Instruct: Given a web search query, retrieve relevant passages that answer the query\nQuery:" + q`.

Measured on a testnet node with the placeholder model (4-core x86 VM with
AVX-512 VNNI): four inputs, 55 tokens, answered in 2.4 s through the stock `openai`
client; all four requests were served in the next mined block.

The gateway has no authentication or billing of its own. Bind it to
localhost, or put it behind a proxy that has them (an HTTP 402 flow with
`contrib/aperture-sdk/aperture_sdk/x402.py` is one option). Test:
`test/functional/feature_embed_gateway.py`.
