#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the consensus-enforced development fund coinbase output.

- getblocktemplate reports the required output and coinbasevalue includes it
- generated blocks pay the fund
- blocks that omit or underpay the fund are rejected with bad-cb-devfund
- the requirement ends at -devfundendheight
"""

from test_framework.blocktools import (
    DEVFUND_REGTEST_SCRIPT,
    NORMAL_GBT_REQUEST_PARAMS,
    create_block,
    create_coinbase,
    devfund_amount,
)
from test_framework.messages import CTxOut
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

DEVFUND_END_HEIGHT = 200


class DevFundTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[f"-devfundendheight={DEVFUND_END_HEIGHT}"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def build_block(self, fund_value):
        node = self.nodes[0]
        tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        height = tmpl["height"]
        coinbase = create_coinbase(height)
        if fund_value is not None:
            coinbase.vout[0].nValue -= fund_value
            coinbase.vout.append(CTxOut(fund_value, DEVFUND_REGTEST_SCRIPT))
            coinbase.rehash()
        block = create_block(tmpl=tmpl, coinbase=coinbase)
        block.solve()
        return block

    def run_test(self):
        node = self.nodes[0]
        address = node.getnewaddress()

        self.log.info("getblocktemplate reports the development fund output")
        tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        assert_equal(tmpl["height"], 1)
        assert_equal(tmpl["coinbasevalue"], 50 * 10**8)
        assert_equal(tmpl["devfund"]["script"], DEVFUND_REGTEST_SCRIPT.hex())
        assert_equal(tmpl["devfund"]["amount"], devfund_amount(1))

        self.log.info("Generated blocks pay the development fund")
        blockhash = node.generatetoaddress(1, address)[0]
        coinbase = node.getblock(blockhash, 2)["tx"][0]
        fund_outputs = [o for o in coinbase["vout"] if o["scriptPubKey"]["hex"] == DEVFUND_REGTEST_SCRIPT.hex()]
        assert_equal(len(fund_outputs), 1)
        assert_equal(int(round(fund_outputs[0]["value"] * 10**8)), devfund_amount(1))
        assert_equal(int(round(coinbase["vout"][0]["value"] * 10**8)), 50 * 10**8 - devfund_amount(1))

        self.log.info("A coinbase without the development fund output is rejected")
        height = node.getblockcount() + 1
        assert_equal(node.submitblock(self.build_block(None).serialize().hex()), "bad-cb-devfund")

        self.log.info("A coinbase that underpays the development fund is rejected")
        assert_equal(node.submitblock(self.build_block(devfund_amount(height) - 1).serialize().hex()), "bad-cb-devfund")

        self.log.info("A coinbase that pays the development fund exactly is accepted")
        block = self.build_block(devfund_amount(height))
        assert_equal(node.submitblock(block.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), block.hash)

        self.log.info("The amount follows the subsidy halving")
        node.generatetoaddress(150 - node.getblockcount(), address)
        tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        assert_equal(tmpl["height"], 151)
        assert_equal(tmpl["devfund"]["amount"], devfund_amount(151))
        assert_equal(devfund_amount(151), 25 * 10**8 * 5 // 100)

        self.log.info("The requirement ends at -devfundendheight")
        node.generatetoaddress(DEVFUND_END_HEIGHT - 1 - node.getblockcount(), address)
        tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        assert_equal(tmpl["height"], DEVFUND_END_HEIGHT)
        assert "devfund" not in tmpl
        block = self.build_block(None)
        assert_equal(node.submitblock(block.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), block.hash)


if __name__ == '__main__':
    DevFundTest().main()
