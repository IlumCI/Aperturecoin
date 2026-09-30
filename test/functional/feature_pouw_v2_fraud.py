#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Fraud proofs for ApertureMatMul v2 embedding results (doc/pouw-v2.md).

On an optimistic chain (-powv2optimistic) nodes accept blocks without
recomputing their embedding results. Each result commits to the forward
pass's per-layer state hashes; a wrong result is proven by a fraud claim that
re-executes a single step (embedding lookup, one layer, or the final norm)
and forfeits the offending block's coinbase to the challenger.

Checks:
- an honest block has no fraud to prove;
- a cheater's wrong lookup commitment, wrong layer commitment and wrong final
  embedding are each accepted optimistically, detected by
  checkblockembeddings, and proven by claims at steps 0, 1 and 3;
- claims are accepted while the coinbase is immature, confirm, and move the
  forfeited coinbase to the challenger;
- tampered claims (wrong state, missing input, wrong step), a second claim on
  the same block, and ordinary spends of the immature coinbase are rejected.
"""

import struct
from decimal import Decimal

from test_framework import aperture_matmulpow_v2 as v2
from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import COIN, CTransaction, CTxIn, CTxOut, COutPoint, FromHex, ToHex, ser_uint256
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

ACTIVATION = 105
RANK = 8


def result_script(txid_hex, vout, embedding, states):
    body = struct.pack("<H", len(states)) + b"".join(states) + embedding
    pushes = [b"APEM", ser_uint256(int(txid_hex, 16)), struct.pack("<I", vout)]
    pushes += [body[k:k + 520] for k in range(0, len(body), 520)]
    return CScript([OP_RETURN] + pushes)


class PoUWFraudTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [[f"-powv2height={ACTIVATION}", "-powv2optimistic"]] * 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def request(self, node, text):
        """Signed request tx (not broadcast): returns (hex, txid, request vout)."""
        req = node.createembeddingrequest(text)
        utxo = next(u for u in node.listunspent() if u["amount"] > 1)
        tx = FromHex(CTransaction(), node.createrawtransaction([{"txid": utxo["txid"], "vout": utxo["vout"]}],
                                                              [{node.getnewaddress(): utxo["amount"] - Decimal("0.001")}]))
        tx.vout.append(CTxOut(0, CScript(bytes.fromhex(req["script"]))))
        raw = node.signrawtransactionwithwallet(ToHex(tx))["hex"]
        return raw, FromHex(CTransaction(), raw).rehash(), 1

    def cheat_block(self, node, raw, txid, embedding, states):
        """A block that serves one request with a caller-chosen (possibly wrong) result."""
        tip = node.getbestblockhash()
        coinbase = create_coinbase(node.getblockcount() + 1, fees=100_000)
        # Two paying outputs, so a claim must take both to forfeit the whole reward.
        half = coinbase.vout[0].nValue // 2
        coinbase.vout[0].nValue -= half
        coinbase.vout.append(CTxOut(half, coinbase.vout[0].scriptPubKey))
        coinbase.vout.append(CTxOut(0, result_script(txid, 1, embedding, states)))
        coinbase.rehash()
        block = create_block(int(tip, 16), coinbase, node.getblock(tip)["mediantime"] + 1, version=0x20000000)
        tx = FromHex(CTransaction(), raw)
        tx.rehash()
        block.vtx.append(tx)
        add_witness_commitment(block)
        block.hashMerkleRoot = block.calc_merkle_root()
        v2.solve(block, RANK, v2.batch_root([(tx.sha256, 1)]))
        return block

    def run_test(self):
        miner, challenger = self.nodes
        addr = miner.getnewaddress()
        miner.generatetoaddress(ACTIVATION + 5, addr)
        self.sync_blocks()

        self.log.info("An honest block has no fraud to prove")
        miner.sendembeddingrequest("honest work")
        honest = miner.generatetoaddress(1, addr)[0]
        self.sync_blocks()
        assert_equal([r["valid"] for r in challenger.checkblockembeddings(honest)], [True])
        assert_raises_rpc_error(-26, "no fraud to prove", challenger.createfraudclaim, honest, 0, challenger.getnewaddress())

        claims = []
        for label, step in (("wrong embedding lookup commitment", 0), ("wrong layer-1 commitment", 1), ("wrong final embedding", 3)):
            self.log.info("Cheater: %s is accepted optimistically", label)
            text = f"request {step}"
            raw, txid, _ = self.request(miner, text)
            honest_result = miner.embed(text)
            emb = bytes.fromhex(honest_result["embedding"])
            states = [bytes.fromhex(h) for h in honest_result["state_hashes"]]
            if step == 3:
                emb = bytes([emb[0] ^ 1]) + emb[1:]
            else:
                states[step] = bytes(32)
            block = self.cheat_block(miner, raw, txid, emb, states)
            assert_equal(miner.submitblock(block.serialize().hex()), None)
            self.sync_blocks()
            report = challenger.checkblockembeddings(block.hash)
            assert_equal((report[0]["valid"], report[0]["fraud_step"]), (False, step))

            self.log.info("The challenger proves it by re-executing step %d", step)
            claim = challenger.createfraudclaim(block.hash, 0, challenger.getnewaddress())
            assert_equal(claim["step"], step)
            claims.append((block, claim))

        block, claim = claims[-1]
        claim_tx = FromHex(CTransaction(), claim["hex"])

        self.log.info("Tampered claims are rejected")
        bad = FromHex(CTransaction(), claim["hex"])
        proof = bytes(bad.vout[1].scriptPubKey)
        bad.vout[1] = CTxOut(0, CScript([OP_RETURN, b"APFP", b"\x00\x00", b"\x02", bytes(8)]))  # step 2, 1-value state
        assert_raises_rpc_error(-26, "fraud-claim-invalid (state has the wrong size)", challenger.sendrawtransaction, bad.serialize().hex())
        bad = FromHex(CTransaction(), claim["hex"])
        bad.vout[1] = CTxOut(0, CScript([OP_RETURN, b"APFP", b"\x00\x00", b"\x01"]))  # step 1 needs a state
        assert_raises_rpc_error(-26, "fraud-claim-invalid (state has the wrong size)", challenger.sendrawtransaction, bad.serialize().hex())
        bad = FromHex(CTransaction(), claim["hex"])
        bad.vout[1] = CTxOut(0, CScript([OP_RETURN, b"APFP", b"\x00\x00", b"\x00"]))  # lookup commitment is honest here
        assert_raises_rpc_error(-26, "fraud-claim-invalid (embedding lookup matches the commitment)", challenger.sendrawtransaction, bad.serialize().hex())
        assert len(proof) > 1000  # the real claim carries the layer-2 output state

        self.log.info("A claim that leaves part of the coinbase to the miner is rejected")
        cb = block.vtx[0]
        bad = CTransaction()
        bad.nVersion = 2
        bad.vin = [CTxIn(COutPoint(cb.sha256, 0))]
        bad.vout = [CTxOut(cb.vout[0].nValue - 100_000, CScript(bytes.fromhex(challenger.getaddressinfo(challenger.getnewaddress())["scriptPubKey"]))),
                    claim_tx.vout[1]]
        assert_equal(len(claim_tx.vin), 2)
        assert_raises_rpc_error(-26, "fraud-claim-incomplete", challenger.sendrawtransaction, bad.serialize().hex())

        self.log.info("An ordinary spend of the immature coinbase is still rejected")
        spend = CTransaction()
        spend.vin = [CTxIn(COutPoint(cb.sha256, 0))]
        spend.vout = [CTxOut(cb.vout[0].nValue - 100_000, CScript(bytes.fromhex(challenger.getaddressinfo(challenger.getnewaddress())["scriptPubKey"])))]
        assert_raises_rpc_error(-26, "bad-txns-premature-spend-of-coinbase", challenger.sendrawtransaction, spend.serialize().hex())

        self.log.info("Valid claims confirm and move each forfeited coinbase to the challenger")
        claim_txids = [challenger.sendrawtransaction(c["hex"]) for _, c in claims]
        self.sync_mempools()
        mined = challenger.generatetoaddress(1, challenger.getnewaddress())[0]
        self.sync_blocks()
        assert set(claim_txids) <= set(miner.getblock(mined)["tx"])
        for (block, c), txid in zip(claims, claim_txids):
            assert miner.gettxout(block.vtx[0].hash, 0) is None and miner.gettxout(block.vtx[0].hash, 1) is None
            paid = challenger.gettxout(txid, 0)
            assert_equal(paid["value"], c["amount"])
            forfeited = block.vtx[0].vout[0].nValue + block.vtx[0].vout[1].nValue
            assert_equal(Decimal(paid["value"]), (Decimal(forfeited) - Decimal("0.001") * COIN) / COIN)

        self.log.info("A second claim on the same block fails: the coinbase is already forfeited")
        again = challenger.createfraudclaim(claims[0][0].hash, 0, challenger.getnewaddress())
        assert_raises_rpc_error(-25, "", challenger.sendrawtransaction, again["hex"])


if __name__ == '__main__':
    PoUWFraudTest().main()
