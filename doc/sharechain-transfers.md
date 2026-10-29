# Freicoin ShareChain, step 2: transferable share credits

```
Status:  Design proposal, not implemented. Builds on doc/sharechain.md (step 1, live on
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
sums the work of every listed fruit per **miner address** (§9 of the spec). So:

- A **fruit** is a claim on future coinbases: `work(f)` of the pool's reward, for every block
  the pool finds while `f` is in the window. Its expected value is `work(f) / work(block) ×
  reward` whatever the window is, because the number of blocks found while `f` is in the window
  is proportional to the window's work.
- A fruit's **owner** is the address its credit goes to. Initially the miner that mined it.
- A **transfer** changes the owner of a set of fruits. PPLNS is unchanged except that it credits
  `owner(f)` instead of `miner(f)`.

This keeps PPLNS as it is (hop-proof, window-bounded, no debt) and makes the accounting
**account-like at the edges** without an account model inside: a transfer moves a whole
position (every fruit of an address up to a height), not individual fruits, and the coinbase
already aggregates per address. The state a verifier adds is one owner per fruit in the window,
20 bytes × at most a few ten thousand fruits, recomputable from the window itself.

Why not balances (an account ledger of FRC owed per address, paid down by each block)? A balance
is a debt: it does not expire, so a miner who leaves, or a burst of hashrate at a lower
difficulty, is owed forever by whoever mines later, and the pool's luck becomes a liability. The
window is what makes p2pool-style payout trust-free; the ledger should sit on top of it, not
replace it.

## 3. Transactions in the share chain

A chain share gets a transaction list beside its fruit list: `txs_root` in the share header
(like `fruits_root`), the bodies in the chain share body (§4.5 of the spec). A transaction is
applied when the chain share that carries it is accepted, after that chain share's fruits are
listed, in list order.

### 3.1 `transfer`

```
version      uint8
from         witness program (the owner: v0 key hash today)
to           witness program
upto_height  uint32    fruits anchored at or below this height
expiry       uint32    chain height after which the transfer lapses (hash-locked only)
hashlock     uint256   optional; zero = unconditional
pubkey       33 bytes  must hash to `from`
signature    64 bytes  Schnorr/ECDSA over the transaction id, by `pubkey`
```

Semantics: every fruit **currently owned by `from`** and anchored at height ≤ `upto_height`
becomes owned by `to`. Unconditional: effective in this chain share. Hash-locked: the fruits are
**locked** (no other transfer by `from` can move them) until a `claim` reveals the preimage or
`expiry` passes, and ownership moves to `to` in the chain share that carries the claim.

### 3.2 `claim`

```
version      uint8
txid         uint256   the hash-locked transfer
preimage     32 bytes  SHA256(preimage) == hashlock
```

Anyone may include it (in practice the buyer does, the moment it learns the preimage).

### 3.3 Rules

1. `pubkey` hashes to `from`; signature valid; `from ≠ to`.
2. `upto_height` ≤ the height of the chain share carrying the transfer (nothing in the future).
3. The transfer moves at least one fruit (a no-op is invalid, so spam costs work: see 3.4).
4. A hash-locked transfer has `expiry` within `MAX_LOCK` (say 60 chain shares, one hour) of the
   carrying chain share; a claim for a lapsed or unknown transfer is invalid.
5. Fruits below the window floor are out of scope: their owner is irrelevant and may be pruned.
   Just above a trusted base (§6.3 of the spec) the node cannot evaluate transfers that touch
   fruits it never saw; it skips them exactly as it skips the payout check there.

Ownership is a function of the chain, so a fork switch recomputes it from the window floor (at
most `WINDOW_SHARES` chain shares; cheap), as PPLNS already is.

### 3.4 Who includes transactions, and why

The miner building a chain share includes transactions from a small pool, like a mempool,
relayed with the `share` messages. There are no fees: a transaction can only move the sender's
own fruits, and a node rate-limits per sender. Market makers have the incentive to run
well-connected nodes; a transfer that no chain-share builder includes for `MAX_LOCK` simply
lapses.

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

1. **Transfers in the share chain** (`transfer`, `claim`, owner lookup in PPLNS, rules of §3,
   RPCs `transfershares`, `claimshares`, `listshareclaims`), unit and functional tests. Chain
   id suffix 05. A few days.
2. **Swap tooling** in `contrib/sharechain/swap/`: a market-maker daemon (quote, verify
   transfers, pay invoices through lnd/CLN, broadcast claims) and a miner-side script (invoice,
   signed transfer). Python, like the regtest miner.
3. Live: Vintage Hash's miners sell to a market maker we run, on mainnet, with real sats.

Steps 1 and 2 need no change to Freicoin consensus, mergemine or bitcoind.
