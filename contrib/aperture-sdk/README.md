ApertureCoin agent SDK
======================

This is a reference Python SDK for agent-facing features. It builds and signs
the transactions the node's 0.21 wallet cannot, namely Taproot script paths.
It reuses the repository's BIP340/BIP341 and serialization code in
`test/functional/test_framework`, so it runs from a repository checkout:

```python
import sys; sys.path.insert(0, "contrib/aperture-sdk")
from aperture_sdk.mandate import Mandate
```

| Module | Purpose | Documentation |
|---|---|---|
| `aperture_sdk.mandate` | Consensus-enforced agent spending mandates | `doc/agent-mandates.md` |
| `aperture_sdk.scriptnum` | Minimal script number encoding | |

The Python secp256k1 code is not constant-time. Use it for reference and
testing only; production agents should sign with libsecp256k1.
