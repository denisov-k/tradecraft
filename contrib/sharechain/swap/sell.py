#!/usr/bin/env python3
"""Sell a miner's share credits to a market maker for lightning sats
(doc/sharechain-transfers.md section 4).

  sell.py --rpc http://127.0.0.1:8638 --rpcuser frc --rpcpassword ... \
          --wif <private key of the mining address> --maker http://maker:9640 --lncli lncli

1. Ask the maker for a quote on this address's credits.
2. Make a preimage; issue a lightning invoice for the quoted sats whose payment hash is its
   SHA256 (lnd: addinvoice --preimage).
3. Publish a hash-locked transfer of the credits to the maker's address (transfershares),
   hashlock = that SHA256, expiry as quoted.
4. Send the maker the txid and the invoice.  It pays once the transfer is in the chain; the
   settlement reveals the preimage; the maker claims.
5. Wait for the invoice to settle (or, with --no-lightning, for the claim to land).

Nothing is given away before being paid: until the claim, the credits stay ours; if the maker
never pays, the lock lapses.
"""
import argparse
import sys
import time

from common import WORK_UNIT, Lncli, Rpc, http_json, new_preimage, sha256_hex


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rpc", required=True)
    ap.add_argument("--rpcuser", required=True)
    ap.add_argument("--rpcpassword", required=True)
    ap.add_argument("--wif", required=True, help="private key (WIF) of the address whose credits are sold")
    ap.add_argument("--address", required=True, help="that address (bech32), to ask for the quote")
    ap.add_argument("--maker", required=True, help="market maker URL, e.g. http://maker:9640")
    ap.add_argument("--lncli", default="lncli")
    ap.add_argument("--no-lightning", action="store_true", help="regtest trust mode: hand the preimage to the maker")
    ap.add_argument("--min-sats", type=int, default=1, help="do not sell below this quote")
    ap.add_argument("--timeout", type=int, default=1800, help="seconds to wait for the payment")
    args = ap.parse_args()

    rpc = Rpc(args.rpc, args.rpcuser, args.rpcpassword)
    quote = http_json(f"{args.maker}/quote?address={args.address}")
    sats = quote["sats"]
    print(f"quote: {sats} sats for {quote['fruits']} fruits ({quote['work'] / WORK_UNIT:.3f} units) up to height {quote['uptoheight']}, lock {quote['expiry']} chain shares")
    if sats < max(args.min_sats, quote.get("minsats", 0)):
        sys.exit("not worth selling yet")

    preimage = new_preimage()
    hashlock = sha256_hex(preimage)
    invoice = None
    if not args.no_lightning:
        ln = Lncli(args.lncli.split())
        invoice = ln.add_invoice(sats, preimage.hex(), f"share credits to {quote['address'][:12]}", args.timeout)
    txid = rpc("transfershares", args.wif, quote["address"], quote["uptoheight"], hashlock, quote["expiry"])
    print(f"transfer {txid} published, locked for {quote['address']}")

    offer = {"txid": txid}
    if invoice:
        offer["invoice"] = invoice
    else:
        offer["preimage"] = preimage.hex()
    http_json(f"{args.maker}/offer", offer)
    print("offer sent; waiting for the payment")

    deadline = time.time() + args.timeout
    while time.time() < deadline:
        if invoice:
            if ln.settled(hashlock):
                print(f"paid: {sats} sats")
                return
        else:
            pending = {p["txid"] for p in rpc("listsharetxs")["pending"]}
            carried = any(c["address"] == quote["address"] for c in rpc("getsharecredits")["credits"])
            if txid not in pending and carried:
                print("claimed (trust mode)")
                return
        time.sleep(10)
    sys.exit("not paid in time; the lock lapses by itself")


if __name__ == "__main__":
    main()
