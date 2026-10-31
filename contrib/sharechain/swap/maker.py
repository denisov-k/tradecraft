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

The price is the only thing the maker decides; everything else is checked against the chain:
the transfer must be pending, to our address, lock at least what was quoted, and have enough
chain shares left before it lapses for the claim to land.
"""
import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

from common import WORK_UNIT, LNbits, Lncli, PaymentInFlight, Rpc, sha256_hex


class Maker:
    def __init__(self, args):
        self.rpc = Rpc(args.rpc, args.rpcuser, args.rpcpassword)
        self.address = args.address
        self.sats_per_unit = args.sats_per_unit
        self.min_sats = args.min_sats
        if args.no_lightning:
            self.lightning = None
        elif args.lnbits:
            self.lightning = LNbits(args.lnbits, args.lnbits_key)
        else:
            self.lightning = Lncli(args.lncli.split(), cltv_limit=args.cltv_limit)
        self.expiry = args.expiry        # chain shares a seller should lock for
        self.min_left = args.min_left    # chain shares that must remain when we pay
        self.max_sats = args.max_sats    # exposure per sale (doc/sharechain-transfers.md §4.1)
        self.lock = threading.Lock()
        self.offers = {}                 # txid -> offer (invoice or preimage)
        self.done = set()

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
        else:
            sats, payment_hash = self.lightning.decode(offer["invoice"])
            if payment_hash != p["hashlock"]:
                return "invoice payment hash is not the transfer's hashlock"
            if sats > worth:
                return f"invoice asks {sats} sats, the credits are worth {worth}"
            if sats > self.max_sats:
                return f"invoice asks {sats} sats, above our limit of {self.max_sats} per sale"
            print(f"paying {sats} sats for {p['fruits']} fruits ({p['work'] / WORK_UNIT:.3f} units)")
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
            if u.path != "/quote":
                return self._reply(404, {"error": "unknown"})
            seller = parse_qs(u.query).get("address", [""])[0]
            try:
                self._reply(200, maker.quote(seller))
            except Exception as e:
                self._reply(500, {"error": str(e)})

        def do_POST(self):
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
    HTTPServer((host, int(port)), Handler).serve_forever()


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
    ap.add_argument("--lnbits-key", default=None, help="that wallet's admin key")
    ap.add_argument("--no-lightning", action="store_true", help="regtest trust mode: sellers send the preimage, nothing is paid")
    ap.add_argument("--listen", default="127.0.0.1:9640", help="HTTP endpoint for sellers")
    args = ap.parse_args()
    if args.lnbits and not args.lnbits_key:
        ap.error("--lnbits needs --lnbits-key")
    maker = Maker(args)
    threading.Thread(target=serve, args=(maker, args.listen), daemon=True).start()
    print(f"market maker {args.address}: {args.sats_per_unit} sats/unit, listening on {args.listen}" + (" (trust mode)" if args.no_lightning else ""))
    try:
        maker.loop()
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
