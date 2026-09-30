#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""ApertureMatMul v2: proof of useful work by protocol-model inference.

doc/pouw-v2.md, doc/protocol-model.md. With -powv2height, regtest runs the
built-in tiny protocol model:

- from the activation height every header carries a ticket over the model's
  real weights (checked by every node), before it v1 headers are required;
- users pay for embedding requests; the miner must serve every request in its
  block with the exact protocol-model embedding, which every validating node
  recomputes;
- the results form a searchable index (searchembeddings);
- an independent Python miner (test_framework/aperture_matmulpow_v2.py) mines
  valid v2 blocks, and blocks with a wrong embedding, a tampered ticket panel
  or the wrong header version are rejected.
"""

import struct
from decimal import Decimal

from test_framework import aperture_matmulpow_v2 as v2
from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import CBlockHeader, CTransaction, CTxOut, FromHex, ToHex, ser_uint256
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

ACTIVATION = 105
RANK = 8
TEXTS = [
    "decentralized proof of useful work",
    "matrix multiplication on tensor cores",
    "ApertureCoin mines embeddings",
]


def result_script(txid_hex, vout, embedding, states):
    """OP_RETURN "APEM" <txid> <vout> <state_count u16 || state hashes || embedding> (doc/pouw-v2.md)."""
    body = struct.pack("<H", len(states)) + b"".join(states) + embedding
    pushes = [b"APEM", ser_uint256(int(txid_hex, 16)), struct.pack("<I", vout)]
    pushes += [body[k:k + 520] for k in range(0, len(body), 520)]
    return CScript([OP_RETURN] + pushes)


class PoUWv2Test(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [[f"-powv2height={ACTIVATION}"]] * 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def request_tx(self, node, script_hex):
        utxo = next(u for u in node.listunspent() if u["amount"] > 1)
        change = utxo["amount"] - Decimal("0.001")
        tx = FromHex(CTransaction(), node.createrawtransaction([{"txid": utxo["txid"], "vout": utxo["vout"]}],
                                                              [{node.getnewaddress(): change}]))
        tx.vout.append(CTxOut(0, CScript(bytes.fromhex(script_hex))))
        return node.signrawtransactionwithwallet(ToHex(tx))["hex"]

    def python_block(self, node, txs=(), results=()):
        tip = node.getbestblockhash()
        height = node.getblockcount() + 1
        coinbase = create_coinbase(height)
        for txid, vout, emb, states in results:
            coinbase.vout.append(CTxOut(0, result_script(txid, vout, emb, states)))
        coinbase.rehash()
        block = create_block(int(tip, 16), coinbase, node.getblock(tip)["mediantime"] + 1, version=0x20000000)
        for raw in txs:
            tx = FromHex(CTransaction(), raw)
            tx.rehash()
            block.vtx.append(tx)
        if txs:
            add_witness_commitment(block)
        block.hashMerkleRoot = block.calc_merkle_root()
        outpoints = [(tx.sha256, n) for tx in block.vtx[1:] for n, o in enumerate(tx.vout) if bytes(o.scriptPubKey)[:6] == b"\x6a\x04APER"]
        v2.solve(block, RANK, v2.batch_root(outpoints))
        return block

    def run_test(self):
        n0, n1 = self.nodes
        addr = n0.getnewaddress()

        self.log.info("Before activation: v1 headers")
        n0.generatetoaddress(ACTIVATION - 1, addr)
        hdr = n0.getblockheader(n0.getbestblockhash())
        assert "powv2" not in hdr and not hdr["version"] & 0x100

        self.log.info("From the activation height every header carries a useful-work ticket")
        n0.generatetoaddress(2, addr)
        self.sync_blocks()
        hdr = n1.getblockheader(n1.getbestblockhash())
        assert hdr["version"] & 0x100
        assert_equal(hdr["powv2"]["panel_bytes"], RANK * 256)
        assert_equal(hdr["height"], ACTIVATION + 1)

        self.log.info("Python and C++ compute the same ticket hash over the real activation panel")
        for h in (ACTIVATION, ACTIVATION + 1):
            bh = n0.getblockhash(h)
            header = FromHex(CBlockHeader(), n0.getblockheader(bh, False))
            root, (op, i, j, s) = header.powv2[:32], struct.unpack("<4H", header.powv2[32:40])
            panel = [b - 256 if b > 127 else b for b in header.powv2[43:]]
            assert any(panel), "a real forward pass has non-zero activations"
            header80 = header.serialize()[:80]
            pow_py = v2.ticket_pow(v2.seed(header80, root), RANK, op, v2.tiny_op(op), i, j, s, panel)
            assert_equal("%064x" % pow_py, n0.getblockheader(bh)["powv2"]["pow_hash"])

        self.log.info("Users pay for embedding requests; the miner must serve them exactly")
        req = n0.createembeddingrequest(TEXTS[0])
        assert_equal(req["tokens"], len(TEXTS[0].encode()))
        txids = [n0.sendrawtransaction(self.request_tx(n0, req["script"]))]
        for text in TEXTS[1:]:
            sent = n0.sendembeddingrequest(text)
            assert_equal(sent["tokens"], len(text.encode()))
            txids.append(sent["txid"])
        block_hash = n0.generatetoaddress(1, addr)[0]
        self.sync_blocks()
        served = n1.getblockembeddings(block_hash)
        assert_equal(sorted(e["txid"] for e in served), sorted(txids))
        for e in served:
            text = TEXTS[txids.index(e["txid"])]
            assert_equal(e["embedding"], n1.embed(text)["embedding"])
            assert_equal(len(e["embedding"]), 2 * 256)
        assert_equal(n0.getblockheader(block_hash)["powv2"]["batch_root"], n1.getblockheader(block_hash)["powv2"]["batch_root"])

        self.log.info("The served embeddings form a searchable index")
        for k, text in enumerate(TEXTS):
            top = n1.searchembeddings(text, 10, 3)
            assert_equal(top[0]["txid"], txids[k])
            assert_greater_than(top[0]["score"], 0.9999)

        self.log.info("Unservable requests are refused")
        assert_raises_rpc_error(-8, "at most 63 tokens", n0.createembeddingrequest, "x" * 64)
        assert_raises_rpc_error(-8, "ids below 257", n0.createembeddingrequest, [300])
        bad = CScript([OP_RETURN, b"APER", bytes([44, 1, 0])]).hex()  # token id 300
        assert_raises_rpc_error(-26, "bad-embed-request", n0.sendrawtransaction, self.request_tx(n0, bad))

        self.log.info("An independent Python miner (zero activation panel) mines a valid v2 block")
        block = self.python_block(n0)
        assert_equal(n0.submitblock(block.serialize().hex()), None)
        assert_equal(n0.getbestblockhash(), block.hash)

        self.log.info("A block whose embedding result is wrong is rejected")
        raw = self.request_tx(n0, n0.createembeddingrequest("useful work")["script"])
        txid = n0.sendrawtransaction(raw)
        emb = n0.embed("useful work")
        good = bytes.fromhex(emb["embedding"])
        states = [bytes.fromhex(h) for h in emb["state_hashes"]]
        assert_equal(len(states), 3)  # tiny model: lookup + 2 layers
        wrong = bytes([good[0] ^ 1]) + good[1:]
        block = self.python_block(n0, [raw], [(txid, 1, wrong, states)])
        assert_equal(n0.submitblock(block.serialize().hex()), "bad-embed-result")
        block = self.python_block(n0, [raw], [])
        assert_equal(n0.submitblock(block.serialize().hex()), "bad-embed-result-count")

        self.log.info("The same block with the exact embedding is accepted")
        block = self.python_block(n0, [raw], [(txid, 1, good, states[:2] + [bytes(32)])])
        assert_equal(n0.submitblock(block.serialize().hex()), "bad-embed-result")
        block = self.python_block(n0, [raw], [(txid, 1, good, states)])
        assert_equal(n0.submitblock(block.serialize().hex()), None)
        assert_equal(n0.getblockembeddings(block.hash)[0]["embedding"], good.hex())
        self.sync_blocks()

        self.log.info("A ticket panel outside the int8 profile range fails proof of work")
        block = self.python_block(n0)
        panel = [0] * (RANK * 256)
        panel[0] = 96  # |a| must be <= 95
        block.powv2 = block.powv2[:40] + block.powv2[40:43] + bytes(v & 0xff for v in panel)
        block.rehash()
        assert_equal(n0.submitblock(block.serialize().hex()), "high-hash")

        self.log.info("A v1 header after activation is rejected")
        block = create_block(int(n0.getbestblockhash(), 16), create_coinbase(n0.getblockcount() + 1),
                             n0.getblock(n0.getbestblockhash())["mediantime"] + 1, version=0x20000000)
        block.solve()
        assert_equal(n0.submitblock(block.serialize().hex()), "bad-powv2-version")


if __name__ == '__main__':
    PoUWv2Test().main()
