#!/usr/bin/env python3
"""Market maker for share credits (doc/sharechain-transfers.md section 4).

Buys miners' PPLNS credits for lightning sats.  Quotes a price per unit of work, watches
the share chain for hash-locked transfers to its address, pays the seller's invoice (whose
payment hash is the transfer's hashlock), and claims the transfer with the preimage the
payment reveals.

  maker.py --rpc http://127.0.0.1:8638 --rpcuser frc --rpcpassword ... \
           --address fc1q... --sats-per-unit 0.05 --lncli lncli --listen 0.0.0.0:9640

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

from common import WORK_UNIT, Lncli, Rpc, sha256_hex


class Maker:
    def __init__(self, args):
        self.rpc = Rpc(args.rpc, args.rpcuser, args.rpcpassword)
        self.address = args.address
        self.sats_per_unit = args.sats_per_unit
        self.min_sats = args.min_sats
        self.lightning = None if args.no_lightning else Lncli(args.lncli.split())
        self.expiry = args.expiry        # chain shares a seller should lock for
        self.min_left = args.min_left    # chain shares that must remain when we pay
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
            print(f"paying {sats} sats for {p['fruits']} fruits ({p['work'] / WORK_UNIT:.3f} units)")
            preimage = self.lightning.pay(offer["invoice"])
        claim = self.rpc("claimshares", txid, preimage)
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
    ap.add_argument("--lncli", default="lncli", help="lncli command (quoted, may include docker exec ...)")
    ap.add_argument("--no-lightning", action="store_true", help="regtest trust mode: sellers send the preimage, nothing is paid")
    ap.add_argument("--listen", default="127.0.0.1:9640", help="HTTP endpoint for sellers")
    args = ap.parse_args()
    maker = Maker(args)
    threading.Thread(target=serve, args=(maker, args.listen), daemon=True).start()
    print(f"market maker {args.address}: {args.sats_per_unit} sats/unit, listening on {args.listen}" + (" (trust mode)" if args.no_lightning else ""))
    try:
        maker.loop()
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
