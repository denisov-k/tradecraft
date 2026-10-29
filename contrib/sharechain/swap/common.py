"""Shared bits of the share-credit swap tools (doc/sharechain-transfers.md section 4).

RPC: freicoind's JSON-RPC over HTTP, standard library only.
Lightning: lnd's `lncli` as a subprocess (the few calls we need: addinvoice, decodepayreq,
payinvoice, lookupinvoice).  `--no-lightning` replaces it with a trust-mode exchange for
regtest stands: the seller hands the preimage over directly.
"""
import base64
import hashlib
import json
import os
import subprocess
import urllib.request

# One unit of work is the work of a difficulty-1 share (2^32 hashes); getsharecredits and
# listsharetxs report `work` as a plain number of hashes.
WORK_UNIT = 2.0 ** 32


class Rpc:
    def __init__(self, url, user, password):
        self.url = url
        self.auth = base64.b64encode(f"{user}:{password}".encode()).decode()
        self.n = 0

    def __call__(self, method, *params):
        self.n += 1
        body = json.dumps({"jsonrpc": "1.0", "id": self.n, "method": method, "params": list(params)}).encode()
        req = urllib.request.Request(self.url, body, {"Content-Type": "application/json", "Authorization": "Basic " + self.auth})
        with urllib.request.urlopen(req, timeout=60) as r:
            reply = json.load(r)
        if reply.get("error"):
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply["result"]


class Lncli:
    """lnd through its CLI.  `cmd` is e.g. ['lncli', '--network=mainnet'] or
    ['docker', 'exec', 'lnd', 'lncli']."""

    def __init__(self, cmd):
        self.cmd = list(cmd)

    def _run(self, *args):
        out = subprocess.run(self.cmd + list(args), capture_output=True, text=True, check=True).stdout
        return json.loads(out)

    def add_invoice(self, sats, preimage_hex, memo, expiry_s):
        r = self._run("addinvoice", "--amt", str(sats), "--preimage", preimage_hex, "--memo", memo, "--expiry", str(expiry_s))
        return r["payment_request"]

    def decode(self, payment_request):
        r = self._run("decodepayreq", payment_request)
        return int(r["num_satoshis"]), r["payment_hash"]

    def pay(self, payment_request):
        # lnd >= 0.11 prints {"payment_preimage": ..., "status": "SUCCEEDED", ...}
        r = self._run("payinvoice", "--force", "--json", payment_request)
        if r.get("status") not in (None, "SUCCEEDED"):
            raise RuntimeError(f"payment {r.get('status')}: {r.get('failure_reason')}")
        return r["payment_preimage"]

    def settled(self, payment_hash):
        r = self._run("lookupinvoice", payment_hash)
        return r.get("state") == "SETTLED" or r.get("settled") is True


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def new_preimage() -> bytes:
    return os.urandom(32)


def http_json(url, payload=None, timeout=30):
    """POST `payload` (or GET when None) and return the JSON reply."""
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data, {"Content-Type": "application/json"} if data else {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)
