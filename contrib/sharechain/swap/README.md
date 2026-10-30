# Share-credit swaps: market maker and seller

Tools for doc/sharechain-transfers.md §4: a miner sells its PPLNS credits to a market maker
for lightning sats, the market maker takes the coinbase payout instead. Python 3, standard
library only; lightning through lnd's `lncli`.

## Market maker

```bash
python3 maker.py --rpc http://127.0.0.1:8638 --rpcuser frc --rpcpassword ... \
    --address fc1q... --sats-per-unit 0.05 --lncli "lncli --network=mainnet" --listen 0.0.0.0:9640
```

- `--address`: where the bought credits are paid (any bech32 address of yours).
- `--sats-per-unit`: the price for one unit of work (a difficulty-1 share, 2³² hashes). A
  share's expected value is `work / block work × reward`, so at a 5 G block difficulty one unit
  is worth `reward / 5e9` FRC; price it in sats from there, minus your margin.
- The maker polls `listsharetxs` for hash-locked transfers to its address and settles the ones
  it has an offer for: pays the invoice if its payment hash is the transfer's hashlock and its
  amount is within the price, then `claimshares` with the preimage the payment returns.
- `--min-left` (chain shares that must remain before the lock lapses) is the maker's safety
  margin for the claim to land; `--expiry` is what it asks sellers to lock for.

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

## Regtest trust mode

`--no-lightning` on both sides: the seller sends the preimage with the offer, the maker claims
without paying. This exercises the chain side (lock, claim, payout to the buyer) on a stand
without a lightning node; nothing in it is safe against a dishonest counterparty.

## What is not here yet

- Quote discovery: the seller must know the maker's URL.
- Several makers, price competition, partial sales.
- The maker's own hedge of its FRC/BTC position.
