#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Consensus rules for native tokens (doc/tokens.md).

Transactions are built with custom scriptPubKeys, signed by the wallet, and
checked with testmempoolaccept; valid ones are then mined. Covers genesis,
fungible conservation, burning, NFT capabilities, malformed prefixes and
coinbase restrictions.
"""

from decimal import Decimal

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CTransaction, CTxOut, FromHex, ToHex
from test_framework.script import CScript
from test_framework.test_framework import BitcoinTestFramework
from test_framework.tokens import token_script
from test_framework.util import assert_equal

TOKEN_VALUE = Decimal("0.0001")


class TokenConsensusTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def fee_utxo(self, exclude=()):
        for u in self.node.listunspent():
            # Never take the fee input from a transaction already spent here: an index-0
            # sibling would otherwise turn a rejected raw genesis into a valid one.
            if u["txid"] not in {t for t, _ in exclude} and u["amount"] > 1:
                return u
        raise AssertionError("no fee utxo")

    def token_utxo(self, category):
        outs = [o for o in self.node.listtokens()["outputs"] if o["tokenData"]["category"] == category]
        assert_equal(len(outs), 1)
        return outs[0]

    def build(self, inputs, outputs):
        """inputs: [(txid, vout)], outputs: [(scriptPubKey hex, value)]. Adds a fee input and change; returns signed hex."""
        fee = self.fee_utxo(exclude=inputs)
        all_inputs = list(inputs) + [(fee["txid"], fee["vout"])]
        total_in = sum(Decimal(str(self.node.gettxout(t, n, True)["value"])) for t, n in all_inputs)
        change = total_in - sum(v for _, v in outputs) - Decimal("0.001")
        vin = [{"txid": t, "vout": n} for t, n in all_inputs]
        tx = FromHex(CTransaction(), self.node.createrawtransaction(vin, [{self.change_addr: change}]))
        for script_hex, value in outputs:
            tx.vout.append(CTxOut(int(value * 10**8), CScript(bytes.fromhex(script_hex))))
        signed = self.node.signrawtransactionwithwallet(ToHex(tx))
        assert signed["complete"], signed
        return signed["hex"]

    def assert_accepted(self, hex_tx):
        result = self.node.testmempoolaccept([hex_tx])[0]
        assert result["allowed"], result
        self.node.sendrawtransaction(hex_tx)
        self.node.generatetoaddress(1, self.miner_addr)

    def assert_rejected(self, hex_tx, reason):
        result = self.node.testmempoolaccept([hex_tx])[0]
        assert not result["allowed"], result
        assert_equal(result["reject-reason"], reason)

    def run_test(self):
        self.node = self.nodes[0]
        self.miner_addr = self.node.getnewaddress()
        self.change_addr = self.node.getnewaddress()
        self.node.generatetoaddress(120, self.miner_addr)
        lock = self.node.getaddressinfo(self.node.getnewaddress())["scriptPubKey"]

        self.log.info("Genesis through the wallet (fungible, mutable NFT, immutable NFT)")
        cat_ft = self.node.tokengenesis(self.node.getnewaddress(), 1000)["category"]
        cat_mut = self.node.tokengenesis(self.node.getnewaddress(), 0, {"capability": "mutable", "commitment": "aa"})["category"]
        cat_imm = self.node.tokengenesis(self.node.getnewaddress(), 0, {"capability": "none", "commitment": "bb"})["category"]
        self.node.generatetoaddress(1, self.miner_addr)
        ft = self.token_utxo(cat_ft)
        assert_equal(ft["tokenData"]["amount"], "1000")
        tx = self.node.decoderawtransaction(self.node.gettransaction(ft["txid"])["hex"])
        assert_equal(tx["vout"][ft["vout"]]["tokenData"]["category"], cat_ft)
        ft_in = [(ft["txid"], ft["vout"])]

        self.log.info("Fungible: inflation, unknown category and NFT ex nihilo are rejected")
        self.assert_rejected(self.build(ft_in, [(token_script(cat_ft, lock, amount=1001), TOKEN_VALUE)]),
                             "bad-txns-token-amount-inflation")
        self.assert_rejected(self.build(ft_in, [(token_script("11" * 32, lock, amount=1), TOKEN_VALUE)]),
                             "bad-txns-token-category")
        self.assert_rejected(self.build(ft_in, [(token_script(cat_ft, lock, amount=1000, nft="none"), TOKEN_VALUE)]),
                             "bad-txns-token-nft-ex-nihilo")

        self.log.info("Malformed prefixes are rejected")
        self.assert_rejected(self.build(ft_in, [(token_script(cat_ft, lock, amount=1000, bitfield_override=0x90), TOKEN_VALUE)]),
                             "bad-txns-token-prefix")
        self.assert_rejected(self.build(ft_in, [(token_script(cat_ft, lock, bitfield_override=0x00), TOKEN_VALUE)]),
                             "bad-txns-token-prefix")

        self.log.info("Fungible: split and exact transfer are accepted")
        self.assert_accepted(self.build(ft_in, [(token_script(cat_ft, lock, amount=600), TOKEN_VALUE),
                                                (token_script(cat_ft, lock, amount=400), TOKEN_VALUE)]))

        self.log.info("Fungible: burning (no token output) is accepted")
        outs = [o for o in self.node.listtokens()["outputs"] if o["tokenData"]["category"] == cat_ft]
        assert_equal(sorted(o["tokenData"]["amount"] for o in outs), ["400", "600"])
        burn = next(o for o in outs if o["tokenData"]["amount"] == "400")
        self.assert_accepted(self.build([(burn["txid"], burn["vout"])], []))
        assert_equal(self.node.listtokens()["balances"][cat_ft]["amount"], "600")

        self.log.info("Mutable NFT: commitment may change, minting may not be created")
        mut = self.token_utxo(cat_mut)
        mut_in = [(mut["txid"], mut["vout"])]
        self.assert_rejected(self.build(mut_in, [(token_script(cat_mut, lock, nft="minting"), TOKEN_VALUE)]),
                             "bad-txns-token-nft-minting")
        self.assert_rejected(self.build(mut_in, [(token_script(cat_mut, lock, nft="none", commitment=b"\x01"), TOKEN_VALUE),
                                                 (token_script(cat_mut, lock, nft="none", commitment=b"\x02"), TOKEN_VALUE)]),
                             "bad-txns-token-nft-ex-nihilo")
        self.assert_accepted(self.build(mut_in, [(token_script(cat_mut, lock, nft="mutable", commitment=b"\xcc\xdd"), TOKEN_VALUE)]))
        assert_equal(self.token_utxo(cat_mut)["tokenData"]["nft"]["commitment"], "ccdd")

        self.log.info("Immutable NFT: commitment cannot change, pass-through is accepted")
        imm = self.token_utxo(cat_imm)
        imm_in = [(imm["txid"], imm["vout"])]
        self.assert_rejected(self.build(imm_in, [(token_script(cat_imm, lock, nft="none", commitment=b"\xbc"), TOKEN_VALUE)]),
                             "bad-txns-token-nft-ex-nihilo")
        self.assert_accepted(self.build(imm_in, [(token_script(cat_imm, lock, nft="none", commitment=b"\xbb"), TOKEN_VALUE)]))

        self.log.info("Raw genesis: only an input spending index 0 creates its txid's category")
        # Two wallet outputs guarantee at least one spendable output at a non-zero index.
        self.node.sendmany("", {self.node.getnewaddress(): 5, self.node.getnewaddress(): 5})
        self.node.generatetoaddress(1, self.miner_addr)
        u1 = next(u for u in self.node.listunspent() if u["vout"] != 0 and u["amount"] > 1)
        self.assert_rejected(self.build([(u1["txid"], u1["vout"])],
                                        [(token_script(u1["txid"], lock, amount=5), TOKEN_VALUE)]),
                             "bad-txns-token-category")
        u0 = next(u for u in self.node.listunspent() if u["vout"] == 0 and u["amount"] > 1)
        max_amount = 2**63 - 1
        self.assert_accepted(self.build([(u0["txid"], 0)], [(token_script(u0["txid"], lock, amount=max_amount), TOKEN_VALUE)]))
        assert_equal(self.node.listtokens()["balances"][u0["txid"]]["amount"], str(max_amount))

        self.log.info("Coinbase outputs cannot carry tokens")
        tip = self.node.getbestblockhash()
        height = self.node.getblockcount() + 1
        coinbase = create_coinbase(height)
        coinbase.vout[0].scriptPubKey = CScript(bytes.fromhex(token_script(tip, "51", amount=1)))
        coinbase.rehash()
        block = create_block(int(tip, 16), coinbase, self.node.getblock(tip)["time"] + 1)
        block.solve()
        assert_equal(self.node.submitblock(block.serialize().hex()), "bad-txns-coinbase-token")


if __name__ == '__main__':
    TokenConsensusTest().main()
