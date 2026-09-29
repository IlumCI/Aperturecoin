#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Agent payments (doc/agent-payments.md): atomic pay-for-result and escrow.

- HTTP 402 quote, funding, settle with key reveal, and result recovery.
- A wrong key cannot settle; refund only after the CSV timeout.
- Escrow: partial capture must return the rest to the payer; refund after timeout.
"""

import os
import sys
from decimal import Decimal

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "contrib", "aperture-sdk"))
from aperture_sdk.mandate import xonly  # noqa: E402
from aperture_sdk.payments import Escrow  # noqa: E402
from aperture_sdk.x402 import Quote, contract_for, make_quote, recover  # noqa: E402

from test_framework.key import generate_privkey  # noqa: E402
from test_framework.messages import CTransaction, CTxOut, FromHex, ToHex  # noqa: E402
from test_framework.script import CScript  # noqa: E402
from test_framework.test_framework import BitcoinTestFramework  # noqa: E402
from test_framework.util import assert_equal  # noqa: E402

FEE = 10_000
TIMEOUT = 10


class AgentPaymentsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def spk(self):
        return bytes.fromhex(self.node.getaddressinfo(self.node.getnewaddress())["scriptPubKey"])

    def fund(self, spk, value):
        utxo = next(u for u in self.node.listunspent() if u["amount"] > 1)
        change = utxo["amount"] - Decimal(value) / 10**8 - Decimal("0.001")
        tx = FromHex(CTransaction(), self.node.createrawtransaction([{"txid": utxo["txid"], "vout": utxo["vout"]}],
                                                                   [{self.node.getnewaddress(): change}]))
        tx.vout.append(CTxOut(value, spk))
        txid = self.node.sendrawtransaction(self.node.signrawtransactionwithwallet(ToHex(tx))["hex"])
        self.node.generatetoaddress(1, self.addr)
        return (txid, 1), CTxOut(value, spk)

    def accept(self, tx):
        result = self.node.testmempoolaccept([ToHex(tx)])[0]
        assert result["allowed"], result
        self.node.sendrawtransaction(ToHex(tx))
        self.node.generatetoaddress(1, self.addr)

    def reject(self, tx, reason):
        result = self.node.testmempoolaccept([ToHex(tx)])[0]
        assert not result["allowed"], result
        assert reason in result["reject-reason"], result

    def run_test(self):
        self.node = self.nodes[0]
        self.addr = self.node.getnewaddress()
        self.node.generatetoaddress(110, self.addr)
        payer, payee = generate_privkey(), generate_privkey()
        payer_spk, payee_spk = self.spk(), self.spk()

        self.log.info("Provider quotes over HTTP 402: encrypted result + sha256(K)")
        result = b'{"forecast": "rain", "confidence": 0.93}'
        quote, key = make_quote(result, 50_000, xonly(payee), TIMEOUT)
        quote = Quote.from_json(quote.to_json())  # round trip through the 402 body
        assert quote.ciphertext != result

        self.log.info("Agent funds the atomic payment contract")
        contract = contract_for(quote, xonly(payer))
        outpoint, spent = self.fund(contract.script_pubkey, quote.amount)

        self.log.info("A wrong key cannot settle; refund is locked until the timeout")
        self.reject(contract.settle(outpoint, spent, payee, b"\x00" * 32, payee_spk, FEE), "EQUALVERIFY")
        self.reject(contract.refund(outpoint, spent, payer, payer_spk, FEE), "non-BIP68-final")

        self.log.info("Provider settles by revealing K; the agent recovers the result from the chain")
        settle = contract.settle(outpoint, spent, payee, key, payee_spk, FEE)
        self.accept(settle)
        on_chain = self.node.getrawtransaction(settle.rehash(), True, self.node.getbestblockhash())
        witness = [bytes.fromhex(x) for x in on_chain["vin"][0]["txinwitness"]]
        assert_equal(recover(quote, witness), result)

        self.log.info("If the provider never settles, the agent refunds after the timeout")
        quote2, _ = make_quote(b"never delivered", 30_000, xonly(payee), TIMEOUT)
        contract2 = contract_for(quote2, xonly(payer))
        outpoint2, spent2 = self.fund(contract2.script_pubkey, quote2.amount)
        self.node.generatetoaddress(TIMEOUT, self.addr)
        self.accept(contract2.refund(outpoint2, spent2, payer, payer_spk, FEE))

        self.log.info("Escrow: the merchant may capture part but must return the rest")
        merchant = generate_privkey()
        merchant_spk = self.spk()
        escrow = Escrow(xonly(payer), xonly(merchant), payer_spk, TIMEOUT, fee_cap=FEE)
        outpoint3, spent3 = self.fund(escrow.script_pubkey, 200_000)
        self.reject(escrow.capture(outpoint3, spent3, merchant, merchant_spk, 150_000, FEE, payer_spk=merchant_spk),
                    "EQUALVERIFY")
        self.reject(escrow.capture(outpoint3, spent3, merchant, merchant_spk, 150_000, FEE + 1), "OP_VERIFY")
        capture = escrow.capture(outpoint3, spent3, merchant, merchant_spk, 150_000, FEE)
        self.accept(capture)
        assert_equal(self.node.gettxout(capture.rehash(), 1)["value"], Decimal(200_000 - 150_000 - FEE) / 10**8)

        self.log.info("Escrow: the payer can take an uncaptured authorization back after the timeout")
        outpoint4, spent4 = self.fund(escrow.script_pubkey, 200_000)
        self.reject(escrow.refund(outpoint4, spent4, payer, FEE), "non-BIP68-final")
        self.node.generatetoaddress(TIMEOUT, self.addr)
        self.accept(escrow.refund(outpoint4, spent4, payer, FEE))


if __name__ == '__main__':
    AgentPaymentsTest().main()
