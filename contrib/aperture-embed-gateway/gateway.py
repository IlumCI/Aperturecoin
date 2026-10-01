#!/usr/bin/env python3
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""OpenAI-compatible embeddings endpoint backed by ApertureCoin useful-work mining.

    POST /v1/embeddings       OpenAI request/response shape, plus an "aperture" object
    GET  /v1/models           the chain's protocol model
    GET  /v1/requests/<txid>:<vout>
                              on-chain status of a settled request

Every embedding is computed twice, by construction identically:
1. at once by the local node with the protocol model (`embed` RPC). The
   integer inference profile is bit-exact, so this already is the value the
   chain will record;
2. by the miners, when the gateway "settles" the input as an on-chain
   embedding request (`sendembeddingrequest`, paid from the node wallet).
   The block that includes the request carries the result, which full nodes
   recompute (or, on optimistic chains, anyone can prove wrong with a fraud
   claim).

The response returns (1) immediately with the request outpoint of (2).
`aperture.wait` (seconds) holds the response until the request is mined and
checks that the on-chain result equals the local one.

Usage:
    gateway.py --rpcurl=http://127.0.0.1:9432/wallet/<name> (--rpccookiefile=F | --rpcuser=U --rpcpassword=P)
               [--tokenizer=tokenizer.json] [--bind=127.0.0.1] [--port=8402]

Text input needs --tokenizer (the protocol model's tokenizer.json, from
contrib/aperture-model/fetch_protocol_model.py) unless the protocol model uses
the byte tokenizer (regtest's tiny model). Token-id input never needs it.
"""

import argparse
import base64
import json
import math
import struct
import sys
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

POLL_SECONDS = 1.0
MAX_WAIT_SECONDS = 3600


class RPCError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


class NodeRPC:
    def __init__(self, url, user=None, password=None, cookiefile=None):
        self.url, self.user, self.password, self.cookiefile = url, user, password, cookiefile

    def _auth(self):
        if self.cookiefile:
            with open(self.cookiefile, encoding="utf8") as f:
                return f.read().strip()
        return f"{self.user}:{self.password}"

    def call(self, method, *params):
        body = json.dumps({"jsonrpc": "1.0", "id": "gw", "method": method, "params": list(params)}).encode()
        req = urllib.request.Request(self.url, data=body, headers={
            "Content-Type": "application/json",
            "Authorization": "Basic " + base64.b64encode(self._auth().encode()).decode(),
        })
        try:
            with urllib.request.urlopen(req, timeout=600) as r:
                reply = json.load(r)
        except urllib.error.HTTPError as e:
            reply = json.load(e)
        if reply.get("error"):
            raise RPCError(reply["error"]["code"], reply["error"]["message"])
        return reply["result"]


def int8_from_hex(h):
    return list(struct.unpack(f"{len(h) // 2}b", bytes.fromhex(h)))


def unit_floats(v):
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


class Gateway:
    def __init__(self, rpc, tokenizer_path=None):
        self.rpc = rpc
        self.tokenizer = None
        if tokenizer_path:
            from tokenizers import Tokenizer
            self.tokenizer = Tokenizer.from_file(tokenizer_path)
        self.model_id = None

    def model(self):
        if self.model_id is None:
            # A zero-length input is rejected, so ask with one token.
            self.model_id = self.rpc.call("embed", "[0]")["model_id"]
        return self.model_id

    def to_rpc_input(self, item):
        """One OpenAI input item (text or token ids) -> the node's input string, token count."""
        if isinstance(item, str):
            if self.tokenizer is None:
                return item, len(item.encode())
            ids = self.tokenizer.encode(item).ids
            return json.dumps(ids), len(ids)
        if isinstance(item, list) and all(isinstance(x, int) and not isinstance(x, bool) for x in item):
            return json.dumps(item), len(item)
        raise ValueError("each input must be a string or an array of token ids")

    @staticmethod
    def split_inputs(inp):
        if isinstance(inp, str):
            return [inp]
        if isinstance(inp, list) and inp and all(isinstance(x, int) and not isinstance(x, bool) for x in inp):
            return [inp]
        if isinstance(inp, list) and inp:
            return inp
        raise ValueError("input must be a string, an array of strings, or token ids")

    def request_status(self, txid, vout):
        tx = self.rpc.call("gettransaction", txid)
        status = {"txid": txid, "vout": vout, "confirmations": tx.get("confirmations", 0)}
        if tx.get("confirmations", 0) <= 0 or "blockhash" not in tx:
            status["status"] = "pending"
            return status, None
        block = tx["blockhash"]
        status.update(status="confirmed", blockhash=block, height=tx.get("blockheight"))
        for res in self.rpc.call("getblockembeddings", block):
            if res["txid"] == txid and res["vout"] == vout:
                return status, res["embedding"]
        status["status"] = "unserved"
        return status, None

    def embeddings(self, body):
        items = self.split_inputs(body.get("input"))
        opts = body.get("aperture") or {}
        settle = bool(opts.get("settle", True))
        wait = min(float(opts.get("wait", 0)), MAX_WAIT_SECONDS)
        fmt = body.get("encoding_format", "float")
        if fmt not in ("float", "base64"):
            raise ValueError("encoding_format must be float or base64")

        out, total = [], 0
        for i, item in enumerate(items):
            rpc_input, _ = self.to_rpc_input(item)
            local = self.rpc.call("embed", rpc_input)
            total += local["tokens"]
            meta = {"model_id": local["model_id"], "int8": local["embedding"], "tokens": local["tokens"]}
            if settle:
                req = self.rpc.call("sendembeddingrequest", rpc_input)
                meta.update(request={"txid": req["txid"], "vout": req["vout"]}, fee=req["fee"], status="pending")
            else:
                meta["status"] = "local"
            out.append((i, local["embedding"], meta))

        if settle and wait > 0:
            deadline = time.time() + wait
            for _, local_hex, meta in out:
                while True:
                    status, onchain = self.request_status(meta["request"]["txid"], meta["request"]["vout"])
                    if status["status"] != "pending" or time.time() >= deadline:
                        break
                    time.sleep(POLL_SECONDS)
                meta.update({k: v for k, v in status.items() if k not in ("txid", "vout")})
                if onchain is not None:
                    meta["matches_local"] = onchain == local_hex

        data = []
        for i, emb_hex, meta in out:
            vec = unit_floats(int8_from_hex(emb_hex))
            emb = vec if fmt == "float" else base64.b64encode(struct.pack(f"<{len(vec)}f", *vec)).decode()
            data.append({"object": "embedding", "index": i, "embedding": emb, "aperture": meta})
        return {"object": "list", "data": data, "model": "aperture-" + self.model()[:16],
                "usage": {"prompt_tokens": total, "total_tokens": total}}


def make_handler(gw):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            sys.stderr.write("gateway: " + (fmt % args) + "\n")

        def reply(self, code, obj):
            raw = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def error(self, code, message, kind="invalid_request_error"):
            self.reply(code, {"error": {"message": message, "type": kind}})

        def do_GET(self):
            try:
                if self.path == "/v1/models":
                    mid = gw.model()
                    quote = gw.rpc.call("estimaterequestfee", "[0]")
                    pricing = {"min_fee_per_token": str(quote["fee_per_token"]), "currency": "SCIENCE",
                               "block_token_budget": quote["block_token_budget"]}
                    return self.reply(200, {"object": "list", "data": [
                        {"id": "aperture-" + mid[:16], "object": "model", "owned_by": "aperturecoin", "model_id": mid,
                         "pricing": pricing}]})
                if self.path.startswith("/v1/requests/"):
                    txid, _, vout = self.path[len("/v1/requests/"):].partition(":")
                    status, onchain = gw.request_status(txid, int(vout))
                    if onchain is not None:
                        status["int8"] = onchain
                        status["embedding"] = unit_floats(int8_from_hex(onchain))
                    return self.reply(200, status)
                self.error(404, "not found")
            except RPCError as e:
                self.error(400, str(e), "node_error")
            except ValueError as e:
                self.error(400, str(e))
            except Exception as e:
                self.error(502, f"node unavailable: {e}", "node_unavailable")

        def do_POST(self):
            if self.path != "/v1/embeddings":
                return self.error(404, "not found")
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"{}")
                if not isinstance(body, dict):
                    raise ValueError("request body must be a JSON object")
                self.reply(200, gw.embeddings(body))
            except RPCError as e:
                self.error(400, str(e), "node_error")
            except (ValueError, TypeError, json.JSONDecodeError) as e:
                self.error(400, str(e))
            except Exception as e:  # node unreachable, bad credentials, ...
                self.error(502, f"node unavailable: {e}", "node_unavailable")

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rpcurl", required=True)
    ap.add_argument("--rpccookiefile")
    ap.add_argument("--rpcuser")
    ap.add_argument("--rpcpassword")
    ap.add_argument("--tokenizer")
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8402)
    args = ap.parse_args()
    if not args.rpccookiefile and not (args.rpcuser and args.rpcpassword):
        ap.error("--rpccookiefile or --rpcuser/--rpcpassword is required")
    gw = Gateway(NodeRPC(args.rpcurl, args.rpcuser, args.rpcpassword, args.rpccookiefile), args.tokenizer)
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(gw))
    print(f"listening on http://{args.bind}:{server.server_address[1]}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
