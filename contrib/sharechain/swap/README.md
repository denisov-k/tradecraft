# Share-credit swaps: market maker and seller

Tools for doc/sharechain-transfers.md §4: a miner sells its PPLNS credits to a market maker
for lightning sats, the market maker takes the coinbase payout instead. Python 3, standard
library only; lightning through lnd's `lncli`.

## Market maker

```bash
python3 maker.py --rpc http://127.0.0.1:8638 --rpcuser frc --rpcpassword ... \
    --address fc1q... --sats-per-unit 0.05 --listen 0.0.0.0:9640 \
    --lnbits https://legend.lnbits.com --lnbits-key <the wallet's admin key>
```

The maker needs a wallet that can pay an invoice and hand back the preimage. No node is
needed: an **LNbits** wallet (hosted or self-hosted, funded from any wallet) does it through
its REST API with `--lnbits` / `--lnbits-key`. With your own lnd use `--lncli "lncli …"`
instead.

- `--address`: where the bought credits are paid (any bech32 address of yours).
- `--sats-per-unit`: the price for one unit of work (a difficulty-1 share, 2³² hashes). A
  share's expected value is `work / block work × reward`, so at a 5 G block difficulty one unit
  is worth `reward / 5e9` FRC; price it in sats from there, minus your margin.
- The maker polls `listsharetxs` for hash-locked transfers to its address and settles the ones
  it has an offer for: pays the invoice if its payment hash is the transfer's hashlock and its
  amount is within the price, then `claimshares` with the preimage the payment returns.
- `--min-left` (chain shares that must remain before the lock lapses) is the maker's safety
  margin for the claim to land; `--expiry` is what it asks sellers to lock for.
- `--max-sats` caps a single purchase (default 20 000). Lightning lets a seller with its own
  node hold the payment and settle only after the lock lapses, keeping both the sats and the
  credits (doc/sharechain-transfers.md §4.1); until the share chain rule there is in, the cap
  is the maker's limit on that loss. Wallets and lightning addresses settle on arrival.
- A payment is sent once. If it has not settled, the maker waits for its preimage and claims
  then, rather than paying again; a claim refused after paying is logged as `LOST`.
- `--cltv-limit` (lnd only, default 40 blocks) bounds how long a payment can stay locked.

## Seller

```bash
python3 sell.py --rpc http://127.0.0.1:8638 --rpcuser frc --rpcpassword ... \
    --wif cV... --address fc1q... --maker http://maker.example:9640 --lncli lncli
```

Asks for a quote, gets an invoice for that amount, publishes the locked transfer with the
invoice's payment hash, sends the maker the txid and the invoice, and waits for the claim.
Run it whenever enough credit has built up (the quote says what it is worth); until the claim
the credits are still yours, and an unpaid lock lapses by itself.

**No lightning node is needed to sell.** The share chain only needs the invoice's payment
hash, and only the wallet that issued the invoice knows the preimage; the maker learns it by
paying. So any wallet works:

- `--lnaddress you@walletofsatoshi.com` (or any LNURL-pay address): the script fetches an
  invoice for the quoted amount itself.
- `--invoice lnbc…`: run with `--quote-only`, make an invoice for that amount in any
  wallet, run again with it.
- `--lncli …`: your own lnd makes the invoice.

Costs: the protocol charges nothing. Receiving on lightning costs whatever your wallet
charges (custodial ones usually nothing; a self-custodial phone wallet may charge a small fee
for inbound capacity the first time).

The WIF is the private key of the mining address (`-defaultshareaddress`). Keep that address
a plain single-key bech32 address (`fc1q…` of 20 bytes, the wallet's `getnewaddress "" bech32`):
that is what the transfer signature covers. `newkey.py` makes one outside the wallet.

## Selling for FRC instead of sats

`frchtlc.py` does the swap with an HTLC on the Freicoin chain (doc/sharechain-transfers.md
§4.2): no lightning, no bitcoin, and no late-settlement problem, since the maker chooses its
timeout.

    # maker: fund the HTLC (its node's wallet pays), keep the state file
    frchtlc.py --rpc http://127.0.0.1:8638/wallet/maker ... offer --seller-pubkey 02..         --frc 1.5 --credits-to fc1q<maker> --state swap1.json > offer.json
    # seller: check it and lock the credits for 30 chain shares
    frchtlc.py --rpc ... lock --offer offer.json --frc-wif K.. --credit-wif K..
    # maker: claim the credits (this reveals the preimage)
    frchtlc.py --rpc ... claim --state swap1.json
    # seller: take the FRC
    frchtlc.py --rpc ... redeem --offer offer.json --transfer <txid> --frc-wif K.. --to fc1q..
    # maker, if the seller never locked: after the timeout
    frchtlc.py --rpc ... refund --state swap1.json --to fc1q..

`newkey.py main` makes the seller's FRC key. The seller locks only while at least
`--min-blocks` (12) Freicoin blocks remain before the maker's refund; the maker claims only
while `--min-left` (5) chain shares remain on the lock.

## BTC for FRC, atomically

`atomicswap.py` trades bitcoin for Freicoin with an HTLC on each chain under one hash: no
exchange, no third party. A (has BTC) picks the secret and locks BTC for long; B (has FRC)
checks it and locks FRC for short; A takes the FRC, which puts the secret on the Freicoin
chain; B finds it there and takes the BTC.

    A: atomicswap.py <rpc options> initiate --b-btc-pubkey 02.. --btc 0.001 --frc 50 --state a.json > offer.json
    B: atomicswap.py <rpc options> participate --offer offer.json --btc-wif K.. --state b.json > reply.json
    A: atomicswap.py <rpc options> redeem-frc --state a.json --reply reply.json --to fc1q..
    B: atomicswap.py <rpc options> redeem-btc --state b.json --btc-wif K.. --to bc1q..
    refunds after the timeouts: refund-btc (A, --state a.json), refund-frc (B, --state b.json)

`<rpc options>` are `--btc-rpc/--btc-rpcuser/--btc-rpcpassword` and the same for `--frc-`.
B demands at least `--min-btc-blocks` (36) bitcoin blocks before A's refund and locks FRC for
`--timeout-blocks` (12) Freicoin blocks; widen the gap if Freicoin blocks are irregular, since
a stalled Freicoin chain delays B's refund but not A's.

## Selling without handing a key to a node

A storefront (a web page talking to a share chain node) can let a miner sell without the
node ever seeing the key: `createsharetransfer <pubkey> <maker address> …` returns the
unsigned transfer and its `sighash` (32 bytes, byte order); the page signs it with the
miner's key (DER ECDSA, e.g. noble-secp256k1 in the browser), `completesharetx` attaches
the signature, `sendsharetx` relays. A newcomer can make the key in the page itself and
point the Bitaxe at its address; `claimshares` needs no key.

## Regtest trust mode

`--no-lightning` on both sides: the seller sends the preimage with the offer, the maker claims
without paying. This exercises the chain side (lock, claim, payout to the buyer) on a stand
without a lightning node; nothing in it is safe against a dishonest counterparty.

## What is not here yet

- Quote discovery: the seller must know the maker's URL.
- Several makers, price competition, partial sales.
- The maker's own hedge of its FRC/BTC position.
