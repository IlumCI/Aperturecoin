#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Development fund as a 2-of-3 Taproot script-path multisig (contrib/devfund).

- contrib/devfund/devfund.py builds tr(H, sortedmulti_a(2, A, B, C));
- coinbases pay the fund (-devfundscript);
- two of three holders spend matured fund outputs with the session flow;
- one signature is not enough: finalize refuses, and a one-signature
  witness is rejected by consensus.
"""

import json
import os
import subprocess
import sys

from test_framework.key import compute_xonly_pubkey, generate_privkey
from test_framework.messages import COIN, CTransaction, FromHex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "contrib", "devfund", "devfund.py")


def tool(*args):
    return subprocess.run([sys.executable, TOOL, *args], check=True, capture_output=True, text=True).stdout


class DevFundMultisigTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.privs = [generate_privkey() for _ in range(2)]
        self.privs.append(bytes.fromhex(json.loads(tool("keygen"))["privkey"]))  # the CSPRNG key generator
        self.xonly = [compute_xonly_pubkey(k)[0].hex() for k in self.privs]
        self.fund = json.loads(tool("script", *self.xonly, "--hrp=rsci"))
        self.extra_args = [[f"-devfundscript={self.fund['script_pubkey']}", "-devfundendheight=1000"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def fund_utxos(self, blocks):
        node = self.nodes[0]
        out = []
        for h in blocks:
            cb = node.getblock(h, 2)["tx"][0]
            for v in cb["vout"]:
                if v["scriptPubKey"]["hex"] == self.fund["script_pubkey"]:
                    out.append((cb["txid"], v["n"], int(v["value"] * COIN)))
        return out

    def run_test(self):
        node = self.nodes[0]
        assert_equal(self.fund["script_pubkey"][:4], "5120")
        assert self.fund["descriptor"].startswith("tr(50929b74")

        self.log.info("Coinbases pay the 2-of-3 Taproot fund")
        blocks = node.generatetoaddress(3, node.getnewaddress())
        node.generatetoaddress(100, node.getnewaddress())  # coinbase maturity
        utxos = self.fund_utxos(blocks)
        assert_equal(len(utxos), 3)
        total = sum(u[2] for u in utxos)
        fee = 20000
        dest = node.getaddressinfo(node.getnewaddress())["scriptPubKey"]
        session = os.path.join(self.options.tmpdir, "spend.json")
        tool("create", *self.xonly, *[f"--utxo={t}:{n}:{a}" for t, n, a in utxos],
             f"--pay={dest}:{total - fee}", f"--fee={fee}", "-o", session)

        self.log.info("One signature is not enough")
        tool("sign", session, f"--privkey={self.privs[1].hex()}")
        fail = subprocess.run([sys.executable, TOOL, "finalize", session], capture_output=True, text=True)
        assert fail.returncode != 0 and "1 signatures, 2 needed" in fail.stderr, fail.stderr

        self.log.info("Two of three holders spend the fund")
        tool("sign", session, f"--privkey={self.privs[2].hex()}")
        final = tool("finalize", session).strip()
        tx = FromHex(CTransaction(), final)
        stack = tx.wit.vtxinwit[0].scriptWitness.stack
        assert_equal(len(stack), 5)  # 3 signature slots, leaf, control block
        assert_equal(sum(1 for item in stack[:3] if item), 2)

        self.log.info("Consensus rejects the same spend with one signature blanked")
        one = FromHex(CTransaction(), final)
        for w in one.wit.vtxinwit:
            first = next(k for k in range(3) if w.scriptWitness.stack[k])
            w.scriptWitness.stack[first] = b""
        res = node.testmempoolaccept([one.serialize().hex()])[0]
        assert not res["allowed"], res
        assert "false/empty top stack element" in res["reject-reason"], res
        assert_raises_rpc_error(-25, "TestBlockValidity failed", node.generateblock, node.getnewaddress(), [one.serialize().hex()])

        txid = node.sendrawtransaction(final)
        node.generatetoaddress(1, node.getnewaddress())
        assert_equal(node.gettransaction(txid)["confirmations"], 1)

        self.log.info("A key outside the fund cannot sign")
        stranger = generate_privkey()
        bad = subprocess.run([sys.executable, TOOL, "sign", session, f"--privkey={stranger.hex()}"], capture_output=True, text=True)
        assert bad.returncode != 0 and "not one of the fund's keys" in bad.stderr


if __name__ == "__main__":
    DevFundMultisigTest().main()
