#!/usr/bin/env python3
"""Sell a miner's share credits to a market maker for lightning sats
(doc/sharechain-transfers.md section 4).

  sell.py --rpc http://127.0.0.1:8638 --rpcuser frc --rpcpassword ... \
          --wif <private key of the mining address> --maker http://maker:9640 --lncli lncli

1. Ask the maker for a quote on this address's credits.
2. Get a lightning invoice for the quoted sats.  Any wallet will do: the invoice's payment
   hash is all the share chain needs, and only the wallet that issued it knows the preimage.
     --lnaddress you@wallet.com   fetches one from a lightning address (LNURL-pay)
     --invoice lnbc...            an invoice you made in any wallet for the quoted amount
     --lncli ...                  your own lnd makes it
3. Publish a hash-locked transfer of the credits to the maker's address (transfershares),
   hashlock = the invoice's payment hash, expiry as quoted.
4. Send the maker the txid and the invoice.  It pays once the transfer is in the chain; your
   wallet's settlement reveals the preimage to the maker; the maker claims.
5. Wait for the credits to be claimed (or, with --lncli, for the invoice to settle).

Nothing is given away before being paid: until the claim, the credits stay ours; if the maker
never pays, the lock lapses.  You never need the preimage; your wallet handles it.
"""
import argparse
import sys
import time

from common import WORK_UNIT, Lncli, Rpc, bolt11_payment_hash, http_json, lnaddress_invoice, new_preimage, sha256_hex


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rpc", required=True)
    ap.add_argument("--rpcuser", required=True)
    ap.add_argument("--rpcpassword", required=True)
    ap.add_argument("--wif", required=True, help="private key (WIF) of the address whose credits are sold")
    ap.add_argument("--address", required=True, help="that address (bech32), to ask for the quote")
    ap.add_argument("--maker", required=True, help="market maker URL, e.g. http://maker:9640")
    ap.add_argument("--lncli", default=None, help="make the invoice with your own lnd (command)")
    ap.add_argument("--lnaddress", default=None, help="fetch the invoice from this lightning address (any wallet that has one)")
    ap.add_argument("--invoice", default=None, help="an invoice from any wallet for the quoted amount (get the quote first with --quote-only)")
    ap.add_argument("--quote-only", action="store_true", help="print the quote and stop")
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
    if args.quote_only:
        return

    preimage = None
    invoice = None
    ln = None
    if args.no_lightning:
        preimage = new_preimage()
        hashlock = sha256_hex(preimage)
    elif args.invoice:
        invoice = args.invoice
        hashlock = bolt11_payment_hash(invoice)
    elif args.lnaddress:
        invoice = lnaddress_invoice(args.lnaddress, sats, "share credits")
        hashlock = bolt11_payment_hash(invoice)
    elif args.lncli:
        ln = Lncli(args.lncli.split())
        preimage = new_preimage()
        hashlock = sha256_hex(preimage)
        invoice = ln.add_invoice(sats, preimage.hex(), f"share credits to {quote['address'][:12]}", args.timeout)
    else:
        sys.exit("say how to get paid: --lnaddress, --invoice, --lncli or --no-lightning")
    txid = rpc("transfershares", args.wif, quote["address"], 0, quote["uptoheight"], 0, hashlock, quote["expiry"])
    print(f"transfer {txid} published, locked for {quote['address']}")

    offer = {"txid": txid}
    if invoice:
        offer["invoice"] = invoice
    else:
        offer["preimage"] = preimage.hex()
    http_json(f"{args.maker}/offer", offer)
    print("offer sent; waiting for the payment")

    def owned():
        return sum(c["fruits"] for c in rpc("getsharecredits")["credits"] if c["address"] == args.address)

    before = owned()
    seen_pending = False
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        if ln is not None:
            if ln.settled(hashlock):
                print(f"paid: {sats} sats")
                return
        else:
            # the transfer is carried (pending), then claimed (gone, credits moved: the maker
            # paid, your wallet has the sats) or lapsed (gone, credits still ours)
            pending = {p["txid"] for p in rpc("listsharetxs")["pending"]}
            if txid in pending:
                seen_pending = True
            elif seen_pending:
                if owned() < before:
                    print("claimed: the maker paid" if invoice else "claimed (trust mode)")
                    return
                sys.exit("the lock lapsed unclaimed")
        time.sleep(5)
    sys.exit("not paid in time; the lock lapses by itself")


if __name__ == "__main__":
    main()
