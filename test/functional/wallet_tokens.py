#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Wallet RPCs for native tokens: tokengenesis, sendtoken, listtokens.

Also checks that ordinary payments never spend (and so never burn) token
outputs.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class WalletTokensTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-fallbackfee=0.0001"], ["-fallbackfee=0.0001"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def balances(self, node):
        return node.listtokens()["balances"]

    def run_test(self):
        alice, bob = self.nodes
        alice.generatetoaddress(110, alice.getnewaddress())
        self.sync_all()

        self.log.info("tokengenesis creates fungible supply plus a minting NFT")
        res = alice.tokengenesis(alice.getnewaddress(), 1_000_000, {"capability": "minting", "commitment": "00"})
        category = res["category"]
        alice.generatetoaddress(1, alice.getnewaddress())
        self.sync_all()
        assert_equal(self.balances(alice)[category], {"amount": "1000000", "nfts": 1})

        self.log.info("sendtoken moves fungible tokens; change keeps the minting NFT")
        alice.sendtoken(bob.getnewaddress(), category, 250_000)
        alice.generatetoaddress(1, alice.getnewaddress())
        self.sync_all()
        assert_equal(self.balances(bob)[category], {"amount": "250000", "nfts": 0})
        assert_equal(self.balances(alice)[category], {"amount": "750000", "nfts": 1})
        minting = [o for o in alice.listtokens()["outputs"] if "nft" in o["tokenData"]]
        assert_equal(minting[0]["tokenData"]["nft"]["capability"], "minting")

        self.log.info("sendtoken can transfer an NFT by commitment")
        genesis = alice.tokengenesis(alice.getnewaddress(), 0, {"capability": "none", "commitment": "c0ffee"})
        art = genesis["category"]
        alice.generatetoaddress(1, alice.getnewaddress())
        alice.sendtoken(bob.getnewaddress(), art, 0, "c0ffee")
        alice.generatetoaddress(1, alice.getnewaddress())
        self.sync_all()
        assert art not in self.balances(alice)
        assert_equal(self.balances(bob)[art], {"amount": "0", "nfts": 1})

        self.log.info("Errors: insufficient balance, unknown NFT, nothing to send")
        assert_raises_rpc_error(-6, "Insufficient token balance", bob.sendtoken, alice.getnewaddress(), category, 250_001)
        assert_raises_rpc_error(-6, "NFT not found", bob.sendtoken, alice.getnewaddress(), art, 0, "00")
        assert_raises_rpc_error(-8, "nothing to send", bob.sendtoken, alice.getnewaddress(), category, 0)

        self.log.info("Ordinary payments never spend token outputs")
        alice.sendtoaddress(bob.getnewaddress(), 1)
        alice.generatetoaddress(1, alice.getnewaddress())
        self.sync_all()
        bob_tokens_before = {(o["txid"], o["vout"]) for o in bob.listtokens()["outputs"]}
        token_value = sum(o["value"] for o in bob.listtokens()["outputs"])
        # Drain every non-token coin; the token outputs must stay untouched.
        bob.sendtoaddress(alice.getnewaddress(), bob.getbalance() - token_value, "", "", True)
        alice.generatetoaddress(1, alice.getnewaddress())
        self.sync_all()
        assert_equal({(o["txid"], o["vout"]) for o in bob.listtokens()["outputs"]}, bob_tokens_before)
        assert_raises_rpc_error(-6, None, bob.sendtoaddress, alice.getnewaddress(), 0.001)

        self.log.info("Token outputs remain spendable once fees are available")
        alice.sendtoaddress(bob.getnewaddress(), 1)
        alice.generatetoaddress(1, alice.getnewaddress())
        self.sync_all()
        bob.sendtoken(alice.getnewaddress(), category, 250_000)
        bob.generatetoaddress(1, bob.getnewaddress())
        self.sync_all()
        assert category not in self.balances(bob)
        assert_equal(self.balances(alice)[category]["amount"], "1000000")


if __name__ == '__main__':
    WalletTokensTest().main()
