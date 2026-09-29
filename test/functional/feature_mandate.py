#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Agent mandates (doc/agent-mandates.md): consensus-enforced spending policies.

An owner funds a mandate vault; an agent key may spend from it only within a
per-period budget, only to allowlisted destinations, and cannot start new
periods after expiry. The owner can sweep at any time. Uses the reference SDK
in contrib/aperture-sdk.
"""

import os
import sys
from decimal import Decimal

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "contrib", "aperture-sdk"))
from aperture_sdk.mandate import Mandate, xonly  # noqa: E402

from test_framework.key import generate_privkey  # noqa: E402
from test_framework.messages import CTransaction, CTxOut, FromHex, ToHex  # noqa: E402
from test_framework.script import CScript  # noqa: E402
from test_framework.test_framework import BitcoinTestFramework  # noqa: E402
from test_framework.util import assert_equal  # noqa: E402

BUDGET = 100_000
PERIOD = 20
FEE = 10_000
VAULT_VALUE = 5 * 10**8


class MandateTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def spk(self):
        return bytes.fromhex(self.node.getaddressinfo(self.node.getnewaddress())["scriptPubKey"])

    def accept(self, tx):
        result = self.node.testmempoolaccept([ToHex(tx)])[0]
        assert result["allowed"], result
        self.node.sendrawtransaction(ToHex(tx))
        self.node.generatetoaddress(1, self.addr)
        return tx.rehash()

    def reject(self, tx, reason):
        result = self.node.testmempoolaccept([ToHex(tx)])[0]
        assert not result["allowed"], result
        assert reason in result["reject-reason"], result

    def agent_spend(self, vault, dest, amount, *, remaining, start, new_start, locktime=None, claim_remaining=None):
        """Build and sign an agent spend; claim_remaining lets a test lie about the old state."""
        md = self.mandate
        claimed = remaining if claim_remaining is None else claim_remaining
        tx, new_remaining = md.spend(vault, dest, amount, FEE, remaining=claimed, period_start=start,
                                     new_start=new_start, locktime=locktime if locktime is not None else self.node.getblockcount())
        spent = md.vault_output(vault[2], remaining, start)
        md.sign_agent(tx, spent, self.agent_key, dest, remaining=claimed, period_start=start,
                      new_start=new_start, amount=amount)
        return tx, new_remaining

    def run_test(self):
        self.node = self.nodes[0]
        self.addr = self.node.getnewaddress()
        self.node.generatetoaddress(110, self.addr)
        owner_key, self.agent_key = generate_privkey(), generate_privkey()
        merchant_a, merchant_b, stranger = self.spk(), self.spk(), self.spk()
        start = self.node.getblockcount()
        expiry = start + 3 * PERIOD

        self.log.info("Owner creates the mandate vault (genesis of the mandate NFT)")
        u0 = next(u for u in self.node.listunspent() if u["vout"] == 0 and u["amount"] > 10)
        self.mandate = Mandate(xonly(owner_key), xonly(self.agent_key), BUDGET, PERIOD, expiry,
                               [merchant_a, merchant_b], max_fee=FEE, category=u0["txid"])
        change = u0["amount"] - Decimal(VAULT_VALUE) / 10**8 - Decimal("0.001")
        tx = FromHex(CTransaction(), self.node.createrawtransaction([{"txid": u0["txid"], "vout": 0}],
                                                                   [{self.node.getnewaddress(): change}]))
        tx.vout.append(self.mandate.vault_output(VAULT_VALUE, BUDGET, start))
        txid = self.node.sendrawtransaction(self.node.signrawtransactionwithwallet(ToHex(tx))["hex"])
        self.node.generatetoaddress(1, self.addr)
        vault = (txid, 1, VAULT_VALUE)

        self.log.info("Agent pays an allowlisted merchant within budget")
        tx, remaining = self.agent_spend(vault, merchant_a, 60_000, remaining=BUDGET, start=start, new_start=start)
        vault = (self.accept(tx), 1, VAULT_VALUE - 60_000 - FEE)
        assert_equal(remaining, 40_000)

        self.log.info("Over budget in the same period is rejected by consensus")
        tx, _ = self.agent_spend(vault, merchant_b, 40_001, remaining=remaining, start=start, new_start=start)
        self.reject(tx, "OP_VERIFY")

        self.log.info("Lying about the remaining budget is rejected")
        tx, _ = self.agent_spend(vault, merchant_b, 90_000, remaining=remaining, start=start, new_start=start,
                                 claim_remaining=BUDGET)
        self.reject(tx, "EQUALVERIFY")

        self.log.info("A non-allowlisted destination is rejected")
        tx, _ = self.agent_spend(vault, merchant_a, 10_000, remaining=remaining, start=start, new_start=start)
        tx.vout[0] = CTxOut(10_000, CScript(stranger))
        self.mandate.sign_agent(tx, self.mandate.vault_output(vault[2], remaining, start), self.agent_key, merchant_a,
                                remaining=remaining, period_start=start, new_start=start, amount=10_000)
        self.reject(tx, "EQUALVERIFY")

        self.log.info("A new period cannot start early")
        tx, _ = self.agent_spend(vault, merchant_b, 50_000, remaining=remaining, start=start,
                                 new_start=start + PERIOD - 1)
        self.reject(tx, "OP_VERIFY")

        self.log.info("Spending the rest of the period's budget works")
        tx, remaining = self.agent_spend(vault, merchant_b, 40_000, remaining=remaining, start=start, new_start=start)
        vault = (self.accept(tx), 1, vault[2] - 40_000 - FEE)
        assert_equal(remaining, 0)

        self.log.info("After the period, the budget resets (CLTV proves the new start height)")
        self.node.generatetoaddress(PERIOD, self.addr)
        new_start = start + PERIOD
        tx, remaining = self.agent_spend(vault, merchant_a, 70_000, remaining=0, start=start, new_start=new_start)
        vault = (self.accept(tx), 1, vault[2] - 70_000 - FEE)
        assert_equal(remaining, 30_000)
        start = new_start

        self.log.info("No new period may start at or after expiry")
        self.node.generatetoaddress(expiry - self.node.getblockcount(), self.addr)
        tx, _ = self.agent_spend(vault, merchant_a, 20_000, remaining=remaining, start=start, new_start=expiry)
        self.reject(tx, "OP_VERIFY")
        # The remaining budget of the last period stays spendable.
        tx, remaining = self.agent_spend(vault, merchant_a, 30_000, remaining=remaining, start=start, new_start=start)
        vault = (self.accept(tx), 1, vault[2] - 30_000 - FEE)

        self.log.info("The owner can sweep the vault at any time (key path)")
        spent = self.mandate.vault_output(vault[2], remaining, start)
        sweep = self.mandate.sweep(vault, spent, owner_key, merchant_a, FEE)
        self.accept(sweep)
        assert_equal(self.node.gettxout(sweep.rehash(), 0)["value"], Decimal(vault[2] - FEE) / 10**8)


if __name__ == '__main__':
    MandateTest().main()
