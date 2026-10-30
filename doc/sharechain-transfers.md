# Freicoin ShareChain, step 2: transferable share credits

```
Status:  Implemented on branch `sharechain` (§3 and §7 step 1, chain id 06); the swap
         tooling (§4, §7 step 2) is not. Builds on doc/sharechain.md (step 1, live on
         mainnet since 2026-10-28). Written in answer to M. Friedenbach's note of 2026-10-29:
         on-chain PPLNS payouts are costly (part of what killed p2pool on bitcoin); let the
         share chain carry transactions so that market makers can buy small miners' shares
         with lightning payments and aggregate them into one coinbase payout.
Author:  Kirill Denisov (denisov-k)
Date:    2026-10-29
```

## 1. Problem

With PPLNS in the coinbase every miner in the window is a coinbase output. On bitcoin p2pool
this meant hundreds of outputs per block, most of them dust that cost more to spend than it was
worth, and a small miner waited for a block to see any income at all. Freicoin's share chain has
the same shape: today the window holds a handful of addresses, but every new small miner is one
more output in every block, and a miner at 0.1 % of the pool gets 0.1 FRC per block in pieces.

The fix Mark proposes: make a miner's share credit a **transferable claim**. A small miner sells
their claim right after mining it, for a lightning payment, to a **market maker** who collects
claims from many miners and takes one large coinbase output when a block comes. The miner gets
paid in minutes with no variance; the coinbase has a few outputs; nobody custodies anything,
because the claim is settled by the block itself.

## 2. What a claim is

Step 1 already keeps the ledger this needs, just not as a ledger: PPLNS walks the window and
sums the work of every listed fruit per **miner address** (§9 of the spec). Step 2 makes it one:

- A **fruit** is a claim on future coinbases: `work(f)` of the pool's reward, for every block
  the pool finds while `f` is in the window. Its expected value is `work(f) / work(block) ×
  reward` whatever the window is, because the number of blocks found while `f` is in the window
  is proportional to the window's work.
- The **ledger** (`ShareLedger`) is work per address and listing height: listing a fruit mints
  its work into its miner's bucket for that height. A bucket keeps its age, so credit still
  leaves with the window; what the ledger adds is that an address's credit is an **amount**
  that can be moved in parts, to anyone.
- A **transfer** moves an amount of credit from one address to another, newest buckets first,
  within a height range. PPLNS credits whatever the ledger says at the anchor.

Credit is a flow, not a stock: a block does not consume it, it pays every bucket in the window
each time, so there is no moment at which "a chunk of shares gets redeemed" that a transfer
would have to race. A transfer only changes who the next block pays. That is what makes an
account model workable here: balances need no settlement step, they age out.

Why not balances in FRC (coin owed per address, paid down by each block)? A balance is a debt:
it does not expire, so a miner who leaves, or a burst of hashrate at a lower difficulty, is
owed forever by whoever mines later, and the pool's luck becomes a liability. The window is
what makes p2pool-style payout trust-free; the ledger sits on top of it.

## 3. Transactions in the share chain

A chain share gets a transaction list beside its fruit list: `txs_root` in the share header
(like `fruits_root`), the bodies in the chain share body (§4.5 of the spec). A transaction is
applied when the chain share that carries it is accepted, after that chain share's fruits are
listed, in list order.

### 3.1 `transfer`

```
type         uint8     1
from         witness program (the owner: a v0 key hash, the only thing a key signs for)
to           witness program (version ≤ 16, 2–40 bytes)
amount       uint256   work to move, in hashes; null = everything in the range
min_height   uint32    buckets listed at or above this chain height
max_height   uint32    ... and at or below this one
expiry       uint32    chain height after which the transfer lapses (hash-locked only, else 0)
hashlock     uint256   SHA256 of the claim's preimage; null = unconditional
pubkey       33 bytes  compressed; `WitnessV0ShortHash(0, pubkey)` (Freicoin's P2WPK program) is `from`
signature    DER ECDSA by `pubkey` over SHA256d("SHARECHAIN-TRANSFER" ‖ CHAIN_ID ‖ the fields above)
```

`txid` = SHA256d of the serialization. Semantics: `amount` of `from`'s credit in the buckets
listed at `min_height..max_height`, taken from the newest bucket down (the buyer gets the credit
with the most window life left; the seller keeps the oldest), becomes `to`'s in the same
buckets, so it keeps its age. Unconditional: effective in this chain share. Hash-locked: the
amount is **held** by the transfer, still paid to `from`, until a `claim` reveals the preimage
(then it is `to`'s) or `expiry` passes (then it is `from`'s again).

### 3.2 `claim`

```
type         uint8     2
transfer_id  uint256   the hash-locked transfer
preimage     uint256   SHA256(preimage) == hashlock
```

Anyone may include it (in practice the buyer does, the moment it learns the preimage).

### 3.3 Rules

Stateless (`CheckShareTxForm`, rejection `chainshare-tx-…`): `from` a v0 key hash, `to` a
witness program, `from ≠ to` [`tx-self-transfer`]; `pubkey` compressed and hashing to `from`
[`tx-bad-pubkey`, `tx-key-mismatch`]; the signature verifies [`tx-bad-signature`]; a hashlock
with an expiry or neither [`tx-bad-lock`]; a claim names a transfer [`tx-bad-claim`].

Chain-dependent (`ShareLedger::Apply`, over the window's ledger after the parent with this
share's fruits minted, `Expire(height)` applied):

1. A transaction id appears at most once in the window [`tx-replay`].
2. `max_height` ≤ the carrying chain share's height [`tx-future-height`]; `min_height ≤
   max_height` [`tx-bad-range`].
3. A hash-locked transfer has `height < expiry ≤ height + MAX_LOCK` [`tx-bad-expiry`].
4. The range holds credit of `from` [`tx-moves-nothing`] and, with an amount, at least that
   much [`tx-insufficient-credit`]; held (locked) credit is not available. A no-op is
   invalid, so spam costs work (3.4).
5. A claim names a transfer still pending, i.e. carried, not claimed and not lapsed
   (`expiry ≥ height`) [`tx-unknown-transfer`], with `SHA256(preimage) = hashlock`
   [`tx-bad-preimage`].
6. At most `MAX_TXS_PER_CHAIN_SHARE` (100) transactions per chain share
   [`chainshare-too-many-txs`], with Merkle root `txs_root` [`chainshare-bad-txs-root`].
7. Where the window walked back from the parent runs into a trusted base (§6.3 of the spec) the
   chain-dependent rules are not applied, as the payout check is not: the node lacks the
   fruits they would touch.

The ledger is a function of the chain: every walk of the window (`ComputeWindowCredits`)
rebuilds it from the window floor by minting fruits and applying transactions chain share by
chain share, so a fork switch needs nothing beyond what PPLNS already does. Every transaction
that touches credit of a window lies inside that window, because it comes at or after the
listing that minted the credit.

### 3.4 Who includes transactions, and why

The miner building a chain share includes transactions from a small pool (1000, like a
mempool), relayed by the `sharetx` P2P message under the share rate limit. A node accepts into
the pool what is valid on its tip behind what the pool already holds, and drops what the new
tip no longer accepts. There are no fees: a transaction can only move the sender's own fruits.
Market makers have the incentive to run well-connected nodes; a transfer that no chain-share
builder includes for `MAX_LOCK` simply lapses.

## 4. The swap

A miner `X` sells its position to a market maker `M` for satoshis over lightning. Standard
hash-lock on one side, lightning HTLC on the other (a submarine swap):

1. `M` publishes a quote: sats per unit of work for fruits anchored at height ≥ `h0`
   (off-chain: an HTTP endpoint, a stratum extension, or the same gossip the shares use).
2. `X` creates a lightning invoice for the agreed amount; its payment hash is `H = SHA256(P)`,
   `X` knows `P`.
3. `X` signs `transfer(X → M, upto_height = now, hashlock = H, expiry = now + MAX_LOCK)` and
   broadcasts it. A chain share includes it; `X`'s fruits are now locked for `M`.
4. `M` sees the transfer in the chain, checks what it covers (work, ages, that nothing is
   double-sold: the lock guarantees it), and pays the invoice.
5. `X` settles the invoice, which reveals `P`. `M` broadcasts `claim(P)`. Ownership moves to `M`.
6. Failure cases. `X` never settles: the HTLC times out, `M` paid nothing, the lock lapses at
   `expiry`. `M` never pays: `X` loses nothing but has its fruits locked for an hour.

**The race Mark points at**: a block can be found between step 5 (lightning settled) and the
claim's inclusion. That block pays `X`, the owner at its anchor, while `M` already paid. The
window is one chain share (60 s) if `M` includes the claim itself; the probability of a block in
it is `60 s / expected block interval` (0.3 % for today's pool, 10 % for a pool with the whole
network), and it is the market maker's risk to price. The alternative (ownership moves at the
transfer, reverts if unclaimed) puts the same race on the miner, who would be paid nothing for
a block found during the lock and then see the sale fail; the professional side should carry
it.

Nothing here needs Freicoin lightning: the sats are bitcoin, which merge-miners already deal in.
The same transfer with an on-chain HTLC (bitcoin or Freicoin script) works for the rest.

## 5. Why this answers the problem

- **Coinbase size.** With market makers active, the window's credit concentrates on a few
  addresses: a block pays the market makers and whoever did not sell. The `MAX_PAYEES` cap and
  the dust rule stay as the backstop.
- **Small miner income.** A miner sells every few chain shares and is paid in sats within
  minutes, at a quoted price with no variance and no waiting for a block. Pool death risk moves
  to the market maker.
- **Trust.** The miner never gives up a claim before being paid (hash lock), the market maker
  never pays for a claim it cannot verify in the chain, the coinbase settles everything.
- **Step 1 untouched.** Shares, windows, PPLNS, validation of payouts: all as deployed. Nodes
  without market makers see one more rule (owner instead of miner) and empty transaction lists.

## 6. Open points

1. `MAX_LOCK` and the transaction pool limits.
2. Whether `to` may be any script (market maker's P2SH/multisig) or a witness program only, as
   miners' addresses are today. Any script works for the coinbase; witness-program-only keeps
   the canonical order rule (§9.5) as is.
3. Quote discovery: off-chain to begin with. A market maker that also runs a stratum endpoint
   can quote in `mining.notify` extensions, but that is a second step.
4. Market makers' own hedge: an FRC claim sold for sats is an FRC/BTC position. That is their
   business, not the protocol's.
5. Mark's fuller idea (a sidechain with general transactions) subsumes this; transfers of fruit
   ownership are the smallest version of it that solves the payout problem, and a sidechain
   could carry them unchanged.

## 7. Plan

1. **Transfers in the share chain** (`transfer`, `claim`, owner lookup in PPLNS, rules of §3),
   RPCs: `transfershares privkey to [amount] [maxheight] [minheight] [hashlock] [expiry]`
   (signs with a WIF key; `amount` in difficulty-1 share units, 0 = all; `expiry` in chain
   shares after the next one), `claimshares txid preimage`, `listsharetxs` (pool and held
   locks); for a key held elsewhere (a storefront signing in the browser):
   `createsharetransfer pubkey to …` → unsigned hex + sighash (32 bytes, byte order),
   `completesharetx hex signature`, `sendsharetx hex`, `decodesharetx hex`; `getsharecredits`
   reports credits by address with their buckets by listing height. Unit test `sharechain_transfers`,
   functional test `feature_sharechain.py`. Chain id suffix 06.
   **Done 2026-10-29.**
2. **Swap tooling** in `contrib/sharechain/swap/`: a market-maker daemon (quote, verify
   transfers, pay invoices through lnd/CLN, broadcast claims) and a miner-side script (invoice,
   signed transfer). Python, like the regtest miner.
3. Live: Vintage Hash's miners sell to a market maker we run, on mainnet, with real sats.

Steps 1 and 2 need no change to Freicoin consensus, mergemine or bitcoind.
