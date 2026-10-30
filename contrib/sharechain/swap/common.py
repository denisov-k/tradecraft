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
import time
import urllib.error
import urllib.parse
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
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                reply = json.load(r)
        except urllib.error.HTTPError as e:
            # freicoind answers an RPC error with HTTP 500 and the error in the body
            try:
                reply = json.load(e)
            except Exception:
                raise RuntimeError(f"{method}: HTTP {e.code}")
        if reply.get("error"):
            raise RuntimeError(f"{method}: {reply['error'].get('message', reply['error'])}")
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


BECH32_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"


def bolt11_payment_hash(invoice: str) -> str:
    """Payment hash (hex, byte order) of a BOLT11 invoice: the tagged field `p`.  Pure
    decoding, no node needed; the signature is not checked (the maker's node does that
    when it pays)."""
    inv = invoice.strip().lower()
    hrp, _, data = inv.rpartition("1")
    if not hrp.startswith("ln") or len(data) < 104 + 7:
        raise ValueError("not a BOLT11 invoice")
    values = [BECH32_CHARSET.index(c) for c in data[:-6]]  # checksum off
    pos = 7  # 35-bit timestamp
    while pos + 3 <= len(values) - 104:  # 104 = 520-bit signature
        tag, length = values[pos], values[pos + 1] * 32 + values[pos + 2]
        pos += 3
        if tag == 1 and length == 52:
            bits = 0
            acc = 0
            out = bytearray()
            for v in values[pos:pos + 52]:
                acc = (acc << 5) | v
                bits += 5
                while bits >= 8:
                    bits -= 8
                    out.append((acc >> bits) & 0xff)
            return bytes(out[:32]).hex()
        pos += length
    raise ValueError("no payment hash in the invoice")


def lnaddress_invoice(address: str, sats: int, comment: str = "") -> str:
    """Fetch an invoice for `sats` from a lightning address (LNURL-pay: user@domain)."""
    name, _, domain = address.partition("@")
    meta = http_json(f"https://{domain}/.well-known/lnurlp/{name}")
    if meta.get("tag") != "payRequest":
        raise ValueError(f"{address}: not a pay request")
    msat = sats * 1000
    if msat < meta.get("minSendable", 0) or msat > meta.get("maxSendable", 1 << 62):
        raise ValueError(f"{address}: {sats} sats outside {meta.get('minSendable', 0) // 1000}..{meta.get('maxSendable', 0) // 1000}")
    sep = "&" if "?" in meta["callback"] else "?"
    url = f"{meta['callback']}{sep}amount={msat}"
    if comment and meta.get("commentAllowed", 0) >= len(comment):
        url += "&comment=" + urllib.parse.quote(comment)
    reply = http_json(url)
    if reply.get("status") == "ERROR":
        raise ValueError(f"{address}: {reply.get('reason')}")
    return reply["pr"]


def bolt11_amount_sats(invoice: str) -> int:
    """Amount of a BOLT11 invoice in sats (0 when the invoice names none)."""
    hrp = invoice.strip().lower().rpartition("1")[0]
    i = 2
    while i < len(hrp) and hrp[i].isalpha():
        i += 1
    digits = ""
    while i < len(hrp) and hrp[i].isdigit():
        digits += hrp[i]
        i += 1
    if not digits:
        return 0
    mult = {"m": 10 ** 8 // 1000, "u": 10 ** 8 // 10 ** 6, "n": 10 ** 8 // 10 ** 9, "p": 10 ** 8 // 10 ** 12}
    unit = hrp[i] if i < len(hrp) else ""
    if unit == "p":
        return int(digits) // 10_000  # 1 sat = 10 000 pBTC
    if unit == "n":
        return int(digits) // 10  # 1 sat = 10 nBTC
    if unit == "u":
        return int(digits) * 100
    if unit == "m":
        return int(digits) * 100_000
    return int(digits) * 100_000_000


class LNbits:
    """An LNbits wallet (hosted or self-hosted) through its REST API: pays invoices and
    hands back the preimage.  No node needed; the wallet's admin key pays."""

    def __init__(self, url, admin_key):
        self.url = url.rstrip("/")
        self.key = admin_key

    def _call(self, method, path, payload=None):
        data = json.dumps(payload).encode() if payload is not None else None
        req = urllib.request.Request(self.url + path, data, {"Content-Type": "application/json", "X-Api-Key": self.key}, method=method)
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            body = e.read().decode(errors="replace")
            raise RuntimeError(f"LNbits {path}: HTTP {e.code} {body[:200]}")

    def decode(self, payment_request):
        return bolt11_amount_sats(payment_request), bolt11_payment_hash(payment_request)

    def pay(self, payment_request):
        r = self._call("POST", "/api/v1/payments", {"out": True, "bolt11": payment_request})
        payment_hash = r.get("payment_hash") or bolt11_payment_hash(payment_request)
        # the preimage comes with the payment record once it is settled
        for _ in range(60):
            p = self._call("GET", f"/api/v1/payments/{payment_hash}")
            preimage = p.get("preimage") or (p.get("details") or {}).get("preimage")
            if p.get("paid") and preimage and int(preimage, 16) != 0:
                return preimage
            time.sleep(2)
        raise RuntimeError("payment not settled within two minutes")

    def settled(self, payment_hash):
        p = self._call("GET", f"/api/v1/payments/{payment_hash}")
        return bool(p.get("paid"))


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
