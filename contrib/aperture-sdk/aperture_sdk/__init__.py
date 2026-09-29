# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""ApertureCoin agent SDK (reference implementation).

Builds and signs transactions for agent mandates (doc/agent-mandates.md) and
agent payments (doc/agent-payments.md). The SDK reuses the BIP340/BIP341,
script and transaction primitives of the repository's Python test framework
(test/functional/test_framework), so it runs from a repository checkout.
"""

import os
import sys

_FRAMEWORK = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "test", "functional"))
if _FRAMEWORK not in sys.path:
    sys.path.insert(0, _FRAMEWORK)
