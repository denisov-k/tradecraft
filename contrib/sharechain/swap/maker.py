#!/usr/bin/env python3
"""Market maker for share credits (doc/sharechain-transfers.md section 4).

Buys miners' PPLNS credits for lightning sats.  Quotes a price per unit of work, watches
the share chain for hash-locked transfers to its address, pays the seller's invoice (whose
payment hash is the transfer's hashlock), and claims the transfer with the preimage the
payment reveals.

  maker.py --rpc http://127.0.0.1:8638 --rpcuser frc --rpcpassword ... \
           --address fc1q... --sats-per-unit 0.05 --listen 0.0.0.0:9640 \
           --lnbits https://legend.lnbits.com --lnbits-key <admin key>      (or --lncli lncli)

Sellers talk to it over HTTP:
  GET  /quote?address=<seller>   -> {"sats": n, "uptoheight": h, "work": w, "fruits": k,
                                     "expiry": e, "address": <maker address>}
  POST /offer {"txid": ..., "invoice": ...}        (lightning)
  POST /offer {"txid": ..., "preimage": ...}       (--no-lightning, regtest trust mode)
  GET  /offers                   -> the price and the completed purchases (a public feed)

and a storefront page (a wallet that signs in the browser; no key ever reaches us) uses:
  GET  /quotes?addresses=a,b,... -> {address: quote} for those with credit
  POST /prepare {"address", "pubkey", "lnaddress" | "invoice", "maxsats"?}
                                 -> the unsigned locked transfer and the 32 bytes to sign
  POST /submit  {"hex", "signature", "invoice"}   -> {"txid"}: relayed, and our offer
  GET  /status?txid=...          -> waiting | locked | paid (we paid and sent the claim) | unknown

The price is the only thing the maker decides; everything else is checked against the chain:
the transfer must be pending, to our address, lock at least what was quoted, and have enough
chain shares left before it lapses for the claim to land.
"""
import argparse
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from common import WORK_UNIT, LNbits, Lncli, PaymentInFlight, Rpc, bolt11_payment_hash, lnaddress_invoice, sha256_hex


class Maker:
    def __init__(self, args):
        self.rpc = Rpc(args.rpc, args.rpcuser, args.rpcpassword)
        self.address = args.address
        self.sats_per_unit = args.sats_per_unit
        self.min_sats = args.min_sats
        if args.no_lightning:
            self.lightning = None
        elif args.lnbits:
            self.lightning = LNbits(args.lnbits, lnbits_key(args))
        else:
            self.lightning = Lncli(args.lncli.split(), cltv_limit=args.cltv_limit)
        self.expiry = args.expiry        # chain shares a seller should lock for
        self.min_left = args.min_left    # chain shares that must remain when we pay
        self.max_sats = args.max_sats    # exposure per sale (doc/sharechain-transfers.md §4.1)
        self.lock = threading.Lock()
        self.offers = {}                 # txid -> offer (invoice or preimage)
        self.done = set()
        self.sales_file = args.sales_file
        self.sales = []                  # completed purchases, newest last (GET /offers)
        if self.sales_file and os.path.exists(self.sales_file):
            with open(self.sales_file) as fh:
                self.sales = [json.loads(line) for line in fh if line.strip()]

    def record_sale(self, sale):
        sale = dict(sale, time=int(time.time()))
        self.sales.append(sale)
        if self.sales_file:
            with open(self.sales_file, "a") as fh:
                fh.write(json.dumps(sale) + "\n")

    def feed(self):
        """Public, read-only: what we buy at and what we bought (for a shop window)."""
        return {"maker": {"address": self.address, "rail": "lightning" if self.lightning else "trust",
                          "satsperunit": self.sats_per_unit, "unit": "difficulty-1 share (2^32 hashes)",
                          "minsats": self.min_sats, "maxsats": self.max_sats, "expiry": self.expiry,
                          "quote": "/quote?address=<your address>", "offer": "POST /offer"},
                "sales": self.sales[-100:]}

    # ---- pricing -------------------------------------------------------------------
    def quote(self, seller):
        info = self.rpc("getsharechaininfo")
        credits = {c["address"]: c for c in self.rpc("getsharecredits")["credits"]}
        c = credits.get(seller)
        work = c["work"] if c else 0.0
        sats = int(work / WORK_UNIT * self.sats_per_unit)
        return {"address": self.address, "sats": sats, "work": work, "fruits": c["fruits"] if c else 0,
                "uptoheight": info["height"], "expiry": self.expiry, "minsats": self.min_sats}

    def price(self, work):
        return int(work / WORK_UNIT * self.sats_per_unit)

    # ---- storefront: the page signs, we build, relay and pay ---------------------------
    def quotes(self, addresses):
        info = self.rpc("getsharechaininfo")
        credits = {c["address"]: c for c in self.rpc("getsharecredits")["credits"]}
        out = {}
        for a in addresses:
            c = credits.get(a)
            if c and c["work"] > 0:
                out[a] = {"sats": self.price(c["work"]), "work": c["work"], "fruits": c["fruits"],
                          "uptoheight": info["height"], "expiry": self.expiry, "minsats": self.min_sats}
        return out

    def prepare(self, req):
        address, pubkey = req["address"], req["pubkey"]
        q = self.quotes([address]).get(address)
        if not q:
            raise ValueError("this address has no credit in the window")
        total_units = q["work"] / WORK_UNIT
        units, sats = 0, q["sats"]  # 0: everything up to the quoted height
        cap = min(int(req.get("maxsats") or 0) or self.max_sats, self.max_sats)
        if cap < sats:  # part of the credit, newest first, at the same rate
            units = int(cap / self.sats_per_unit)
            if units <= 0:
                raise ValueError(f"{cap} sats do not buy a whole unit")
            sats = min(int(units * self.sats_per_unit), cap)
        if sats < max(1, self.min_sats):
            raise ValueError(f"worth {sats} sats, below our minimum of {self.min_sats}")
        if self.lightning is None:
            invoice, hashlock = None, req["hashlock"]
        else:
            invoice = req.get("invoice") or lnaddress_invoice(req["lnaddress"], sats, "share credits")
            got, hashlock = self.lightning.decode(invoice)
            if got != sats:
                raise ValueError(f"the invoice is for {got} sats, the sale for {sats}")
        r = self.rpc("createsharetransfer", pubkey, self.address, units, q["uptoheight"], 0, hashlock, self.expiry)
        if r["from"] != address:
            raise ValueError("that public key is not this address's")
        return {"hex": r["hex"], "sighash": r["sighash"], "from": r["from"], "sats": sats,
                "units": units or total_units, "invoice": invoice, "hashlock": hashlock}

    def submit(self, req):
        signed = self.rpc("completesharetx", req["hex"], req["signature"])
        d = self.rpc("decodesharetx", signed["hex"])
        if d.get("type") != "transfer" or d.get("to") != self.address or not d.get("hashlock"):
            raise ValueError("not a hash-locked transfer to us")
        if self.lightning is None:
            offer = {"preimage": req["preimage"]}
            if sha256_hex(bytes.fromhex(req["preimage"])) != d["hashlock"]:
                raise ValueError("preimage does not match")
        else:
            offer = {"invoice": req["invoice"]}
            if bolt11_payment_hash(req["invoice"]) != d["hashlock"]:
                raise ValueError("the invoice is not for this transfer's hashlock")
        txid = self.rpc("sendsharetx", signed["hex"])
        with self.lock:
            self.offers[txid] = offer
        print(f"storefront offer {txid[:16]} from {d['from']}")
        return {"txid": txid}

    def status(self, txid):
        if txid in self.done:
            sale = next((s for s in reversed(self.sales) if s.get("transfer") == txid), None)
            return {"state": "paid", "sale": sale}
        lists = self.rpc("listsharetxs")
        for p in lists["pending"]:
            if p["txid"] == txid:
                return {"state": "locked", "expiry": p["expiry"], "height": self.rpc("getsharechaininfo")["height"],
                        "paying": bool(self.offers.get(txid, {}).get("paid_hash"))}
        if any(t["txid"] == txid for t in lists["pool"]):
            return {"state": "waiting"}
        return {"state": "unknown"}

    # ---- the swap ------------------------------------------------------------------
    def pending(self):
        return {p["txid"]: p for p in self.rpc("listsharetxs")["pending"] if p["to"] == self.address}

    def settle(self, txid, offer):
        if offer.get("paid_hash"):
            return self.finish(txid, offer)
        p = self.pending().get(txid)
        if not p:
            return "not pending (not carried yet, claimed, or lapsed)"
        height = self.rpc("getsharechaininfo")["height"]
        if p["expiry"] - height < self.min_left:
            return f"only {p['expiry'] - height} chain shares left before it lapses"
        worth = self.price(p["work"])
        if worth < self.min_sats:
            return f"worth {worth} sats, below the minimum {self.min_sats}"
        if self.lightning is None:
            preimage = offer["preimage"]
            if sha256_hex(bytes.fromhex(preimage)) != p["hashlock"]:
                return "preimage does not match the hashlock"
            print(f"[trust mode] would pay {worth} sats for {p['fruits']} fruits ({p['work'] / WORK_UNIT:.3f} units)")
            offer["sale"] = {"transfer": txid, "seller": p["from"], "sats": worth, "units": p["work"] / WORK_UNIT, "fruits": p["fruits"]}
        else:
            sats, payment_hash = self.lightning.decode(offer["invoice"])
            if payment_hash != p["hashlock"]:
                return "invoice payment hash is not the transfer's hashlock"
            if sats > worth:
                return f"invoice asks {sats} sats, the credits are worth {worth}"
            if sats > self.max_sats:
                return f"invoice asks {sats} sats, above our limit of {self.max_sats} per sale"
            print(f"paying {sats} sats for {p['fruits']} fruits ({p['work'] / WORK_UNIT:.3f} units)")
            offer["sale"] = {"transfer": txid, "seller": p["from"], "sats": sats, "units": p["work"] / WORK_UNIT, "fruits": p["fruits"]}
            # From here on the sats may be gone: never pay this offer again, only wait for
            # the preimage (a slow or holding seller) and claim.
            offer["paid_hash"] = payment_hash
            offer["expiry"] = p["expiry"]
            try:
                preimage = self.lightning.pay(offer["invoice"])
            except PaymentInFlight:
                return "paid, waiting for the seller to settle"
        return self.claim(txid, offer, preimage)

    def finish(self, txid, offer):
        """A payment we already sent: claim once it has settled."""
        preimage = self.lightning.sent_preimage(offer["paid_hash"])
        if not preimage:
            height = self.rpc("getsharechaininfo")["height"]
            return f"paid, seller has not settled; the lock lapses in {offer['expiry'] - height} chain shares"
        return self.claim(txid, offer, preimage)

    def claim(self, txid, offer, preimage):
        try:
            claim = self.rpc("claimshares", txid, preimage)
        except Exception as e:
            if offer.get("paid_hash"):
                # Paid and refused: the seller settled after the lock lapsed (§4.1).
                print(f"LOST {txid}: paid but the claim was refused ({e}); seller settled too late")
                self.done.add(txid)
            raise
        print(f"claimed {txid} with {claim}")
        if offer.get("sale"):
            self.record_sale(dict(offer["sale"], claim=claim))
        return None

    def loop(self):
        while True:
            with self.lock:
                todo = [(t, o) for t, o in self.offers.items() if t not in self.done]
            for txid, offer in todo:
                try:
                    err = self.settle(txid, offer)
                except Exception as e:  # RPC or lightning failure: retry next round
                    err = f"error: {e}"
                if err is None:
                    self.done.add(txid)
                else:
                    print(f"{txid[:16]}: {err}")
            time.sleep(5)


def lnbits_key(args):
    if args.lnbits_key_file:
        with open(args.lnbits_key_file) as fh:
            return fh.read().strip()
    return os.environ.get("LNBITS_ADMIN_KEY") or args.lnbits_key


def serve(maker, listen):
    class Handler(BaseHTTPRequestHandler):
        def _reply(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            if u.path in ("/offers", "/offers.json"):
                return self._reply(200, maker.feed())
            try:
                if u.path == "/quotes":
                    addrs = [a for a in q.get("addresses", [""])[0].split(",") if a][:100]
                    return self._reply(200, maker.quotes(addrs))
                if u.path == "/status":
                    return self._reply(200, maker.status(q.get("txid", [""])[0]))
            except Exception as e:
                return self._reply(500, {"error": str(e)})
            if u.path != "/quote":
                return self._reply(404, {"error": "unknown"})
            seller = parse_qs(u.query).get("address", [""])[0]
            try:
                self._reply(200, maker.quote(seller))
            except Exception as e:
                self._reply(500, {"error": str(e)})

        def do_POST(self):
            if self.path in ("/prepare", "/submit"):
                try:
                    req = json.loads(self.rfile.read(min(int(self.headers.get("Content-Length", "0")), 65536)))
                    return self._reply(200, maker.prepare(req) if self.path == "/prepare" else maker.submit(req))
                except (KeyError, ValueError) as e:
                    return self._reply(400, {"error": str(e)})
                except Exception as e:
                    return self._reply(500, {"error": str(e)})
            if self.path != "/offer":
                return self._reply(404, {"error": "unknown"})
            try:
                offer = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
                txid = offer["txid"]
                assert "invoice" in offer or "preimage" in offer
            except Exception:
                return self._reply(400, {"error": "offer needs txid and invoice (or preimage)"})
            with maker.lock:
                maker.offers[txid] = offer
            print(f"offer for {txid[:16]} received")
            self._reply(200, {"accepted": True})

        def log_message(self, *a):
            pass

    host, port = listen.rsplit(":", 1)
    ThreadingHTTPServer((host, int(port)), Handler).serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rpc", required=True, help="freicoind RPC URL, e.g. http://127.0.0.1:8638")
    ap.add_argument("--rpcuser", required=True)
    ap.add_argument("--rpcpassword", required=True)
    ap.add_argument("--address", required=True, help="our Freicoin address: where the bought credits are paid")
    ap.add_argument("--sats-per-unit", type=float, required=True, help="sats paid per unit of work (one difficulty-1 share)")
    ap.add_argument("--min-sats", type=int, default=100, help="smallest purchase")
    ap.add_argument("--expiry", type=int, default=30, help="chain shares a seller should lock for (quoted)")
    ap.add_argument("--min-left", type=int, default=5, help="chain shares that must remain before the lock lapses when we pay")
    ap.add_argument("--max-sats", type=int, default=20000, help="largest single purchase: a seller who settles after the lock lapses keeps both (doc/sharechain-transfers.md 4.1)")
    ap.add_argument("--cltv-limit", type=int, default=40, help="lnd: largest total CLTV (bitcoin blocks) a payment may lock")
    ap.add_argument("--lncli", default="lncli", help="pay with your own lnd: lncli command (quoted, may include docker exec ...)")
    ap.add_argument("--lnbits", default=None, help="pay from an LNbits wallet instead: its URL, e.g. https://legend.lnbits.com")
    ap.add_argument("--lnbits-key", default=None, help="that wallet's admin key (prefer --lnbits-key-file: arguments show in ps)")
    ap.add_argument("--lnbits-key-file", default=None, help="a file holding the admin key (or set LNBITS_ADMIN_KEY)")
    ap.add_argument("--sales-file", default=None, help="append completed purchases here (JSON lines), served at GET /offers")
    ap.add_argument("--no-lightning", action="store_true", help="regtest trust mode: sellers send the preimage, nothing is paid")
    ap.add_argument("--listen", default="127.0.0.1:9640", help="HTTP endpoint for sellers")
    args = ap.parse_args()
    if args.lnbits and not (args.lnbits_key or args.lnbits_key_file or os.environ.get("LNBITS_ADMIN_KEY")):
        ap.error("--lnbits needs its admin key: --lnbits-key-file, LNBITS_ADMIN_KEY or --lnbits-key")
    maker = Maker(args)
    threading.Thread(target=serve, args=(maker, args.listen), daemon=True).start()
    print(f"market maker {args.address}: {args.sats_per_unit} sats/unit, listening on {args.listen}" + (" (trust mode)" if args.no_lightning else ""))
    try:
        maker.loop()
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
