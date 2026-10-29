# Freicoin ShareChain: a p2pool-style decentralized payout for solo merge-miners

```
Status:  Implemented on branch `sharechain` (freicoind 28.1 + the tradecraft:tmp skeleton,
         src/sharechain.{h,cpp}, M. Friedenbach, May–Nov 2023); regtest and functional tests
         pass; mainnet parameters not yet exercised on real hashrate (§12, §14)
Layer:   Node / mining (no consensus change: a share is a regular Freicoin block template;
         payouts are ordinary coinbase outputs)
Author:  Kirill Denisov (denisov-k)
Based on: the ShareChain skeleton above; mergemine v29.0.0.1 stratum/aux-pow protocol;
          p2pool (Forrest Voight, 2011) for the share-chain and PPLNS mechanics;
          FruitChains (Pass & Shi, 2017) for crediting off-chain work, see
          doc/sharechain-uncles.md
Date:    2026-10-23
```

## 1. Abstract

A **share chain** lets independent merge-miners pool their Freicoin rewards without a pool
operator. Each participant runs the ordinary self-hosted stack (freicoind + mergemine
bitcoind + their own SHA256 hardware). Their freicoind builds Freicoin block templates whose
coinbase already pays every recent contributor in proportion to work (PPLNS). A **share** is
such a template proven by a header hash that meets a share target far below the Freicoin
target. Shares are gossiped and validated by all participants; one share in `M` also meets the
**chain target** and becomes a **chain share**, which links the chain and records (lists) the
shares seen since. Credit is earned by being listed, not by being on the chain, so propagation
races on the chain do not cost anyone pay. When a share also meets the Freicoin target it is a
Freicoin block, and the payout has already happened inside its coinbase. Nobody holds funds,
nobody can redirect them, and a participant who leaves keeps whatever the window still owes.

This document fixes the share format, validation rules, chain selection, crediting rule, share
difficulty adjustment and the PPLNS payout function, with test vectors, as implemented.

## 2. Design principles

1. **Reuse the aux-pow machinery.** The share commitment travels in the Freicoin coinbase like
   the block-final commitment, and the proof that a template was merge-mined is the existing
   `AuxProofOfWork`. bitcoind/mergemine needs no change.
2. **Shares are self-contained.** A share carries everything needed to verify it except the
   share-chain ancestry it references, so a verifier never needs the sender's mempool or
   template.
3. **Payout is the coinbase.** No pool wallet, no payout transactions, no debt.
4. **Verify proportions, not totals.** The total paid is read from the share itself; the rule
   only constrains how it is split.
5. **Same binary for solo and pooled mining.** A chain with one participant pays that
   participant everything; joining the main share chain is a configuration switch.
6. **No consensus change.** A node that ignores share chains validates the resulting blocks as
   usual.
7. **One hash, two roles (2-for-1).** Every hash attempt is a share candidate and a chain-share
   candidate; a miner never chooses between earning and extending the chain.

## 3. Notation and constants

| Name | main | regtest | Meaning |
|---|---|---|---|
| `CHAIN_ID` | `"SHARECHAIN-MAIN" ‖ 00… ‖ 06` | `"SHARECHAIN-REGTEST" ‖ 00… ‖ 06` | `share_chain_path`: key of the share commitment in the coinbase Merkle hash map (uint256) |
| `COMMIT_ID` | `4b 4a 49 48` | same | 4-byte tag after the commitment root in the coinbase (skeleton) |
| `SHARE_BITS` | `0x1b068db2` (difficulty 10 000) | `0x207fffff` | initial share target; also the easiest target the chain allows |
| `M` (`CHAIN_FACTOR`) | 6 | 4 | chain target = share target / `M` |
| `R` (`RECENCY`) | 10 | 10 | a fruit stays listable for `R` chain heights after its anchor |
| `SHARE_INTERVAL` | 10 s | n/a | target spacing of shares (retarget on main only) |
| `WINDOW_K` | 4 | 2²⁰ | PPLNS window = `WINDOW_K` × work(Freicoin target of the template); on regtest a block is two hashes, so the window is `WINDOW_SHARES` there |
| `WINDOW_SHARES` | 4320 | 48 | the window spans at most this many chain shares (3 days at 60 s on main) |
| `MAX_LOCK` | 60 | 60 | a hash-locked transfer lapses at most this many chain shares after its inclusion (doc/sharechain-transfers.md) |
| `MAX_PAYEES` | 1000 | 1000 | maximum payout outputs in a coinbase (bounded by the block, not by the share: §4.4) |
| `DUST` | 1000 kria | 1000 | outputs below this are dropped and redistributed |
| `MAX_TEMPLATE_AGE` | 2 | 2 | a live share must build on one of the last `MAX_TEMPLATE_AGE` Freicoin blocks |
| `work(bits)` | `2^256 / (target + 1)` | | work of a share, same formula as block chainwork |

Terms: a **fruit** is any valid share (every share is a fruit); a **chain share** is a fruit whose
proof-of-work also meets the chain target; a fruit's **anchor** is the chain share it hangs from.
All integers are little-endian. `VARINT` is Bitcoin Core's VARINT, `CompactSize` the P2P
vector-length encoding.

## 4. Share format

A share is `ShareHeader ‖ ShareWitness`. The header is what the coinbase commits to and what
the chain links; the witness proves the header was committed in a mined template.

### 4.1 ShareHeader

| Field | Type | Bytes | Meaning |
|---|---|---|---|
| `version` | uint32 | 4 | share format version (0) |
| `bits` | uint32 | 4 | compact share target this share had to meet (§7.3) |
| `height` | uint32 | 4 | anchor height + 1; 0 for a share hanging from nothing (empty chain) |
| `total_work` | uint256 | 32 | `anchor.total_work + work(anchor.bits)`; zero at height 0 |
| `anchor` | uint256 | 32 | hash of the chain share this share hangs from; null at height 0 |
| `prev_root` | uint256 | 32 | MMR root of all chain shares up to and including the anchor |
| `fruits_root` | uint256 | 32 | Merkle root of the sorted fruit list this template records (§6.2); null if empty |
| `txs_root` | uint256 | 32 | Merkle root of the share chain transactions this template carries (doc/sharechain-transfers.md); null if none |
| `miner` | witness program | CompactSize version + program | payout address of this share's miner |

**Header hash** = SHA256d over the fields in this order (`Share::GetHeaderHash`); 193 bytes for
a 20-byte program. Only the MMR root enters the header; the MMR peaks travel with chain shares
(§4.5), so verifiers of fruits never need them. The fruit list is committed in *every* header
because the template is hashed before anyone knows whether it will reach the chain target.

### 4.2 ShareWitness

| Field | Type | Meaning |
|---|---|---|
| `flags` | uint8 | bit 0 (`FLAG_AUX`): merge-mined share, `aux` follows |
| `aux` | `AuxProofOfWork` | only with `FLAG_AUX`: the Freicoin header's aux-pow exactly as freicoind received it (commit fields, secret, midstate, commit branch, aux branch, parent header) |
| `commit` | VARINT n, then n × (uint8 skip bits, uint256 skip hash) | Merkle-hash-map branch from the header hash to the commitment root, key `CHAIN_ID` |
| `cb_hash` | uint256 | txid of the coinbase: the Merkle leaf, and what the coinbase rebuilt by the verifier must hash to |
| `cb_version` | uint32 | the coinbase's transaction version (2) |
| `script_sig` | VARINT len + bytes | the coinbase input script: height, extranonce, miner tag |
| `sequence` | uint32 | the coinbase input's sequence |
| `total` | int64 | total value of the coinbase outputs (subsidy plus fees): what PPLNS splits |
| `nLockTime` | uint32 | the coinbase's lock time |
| `lock_height` | uint32 | the coinbase's lock height (version-2 transactions serialize it after `nLockTime`; templates build version-2 coinbases) |
| `branch` | VARINT n + n × uint256 | Merkle path from the coinbase (leaf 0) to the Freicoin block's Merkle root |
| `nVersion, hashPrevBlock, share_chain_path, nTime, nBits, nNonce` | 4, 32, 32, 4, 4, 4 | Freicoin header fields; `hashMerkleRoot` is derived, `share_chain_path` (= `CHAIN_ID`) sits in its slot |

Native shares (mined on freicoind's own stratum) have `flags = 0`. Aux shares differ only by
the flag and the `AuxProofOfWork`; the Freicoin header fields are the same fields.

The coinbase itself is **not** carried: its outputs are the PPLNS payouts every verifier
computes from the share's anchor (§9) followed by the share commitment, so a share has the
same size whether the window holds two miners or a thousand. The verifier rebuilds the
coinbase from the fields above and its own payouts and checks the txid against `cb_hash`
(§6.1 rule 7); `cb_hash` itself is bound by the proof of work through the Merkle root.

### 4.3 Reconstruction (`Share::GetBlockHeader`, `BuildShareCoinbase`)

Header, from the witness alone:

1. `merkle` = `ComputeMerkleRootFromBranch(cb_hash, branch, 0)`.
2. Freicoin header = (`nVersion, hashPrevBlock, merkle, nTime, nBits, nNonce`), with `aux` as
   its aux-pow when `FLAG_AUX`.

Coinbase, by the verifier (§6.1 rule 7):

1. `h0` = header hash (§4.1); `root` = `ComputeMerkleMapRootFromBranch(h0, commit, share_chain_path)`.
2. outputs = `PPLNS(A, total, miner)` (§9) in canonical order, then `OP_RETURN <root ‖ COMMIT_ID>`.
3. coinbase = (`cb_version`, one input `(null, script_sig, sequence)`, outputs, `nLockTime`,
   `lock_height`); its txid must equal `cb_hash`.

**PoW hash** (`Share::GetPoWHash`) = the hash of that Freicoin header for a native share; for an
aux share the *parent (Bitcoin) header hash* reached through the aux-pow
(`GetAuxiliaryHash().first`), the same hash the Freicoin consensus compares with the aux target.
Share and chain targets apply to the PoW hash.

**Share hash** (`Share::GetHash`) = the PoW hash. This is what `anchor` and the fruit lists refer
to, and what makes a share unique: the Freicoin header of a merge-mining job is the same for
every parent hash the miner finds, so the parent hash has to be the identity (the first live run
rejected every submission after the first in a job as a duplicate before this was fixed).

### 4.4 Size

Header 161 B; witness ≈ 180 B (coinbase fields ≈ 60 B plus the ≈ 40 B input script) + 32 B
per Merkle-branch level; aux-pow ≈ 440 B. A native share ≈ 0.4 KB, an aux share ≈ 0.9 KB,
independent of the number of payees. The block carries the outputs: 31 B each, 31 KB at
`MAX_PAYEES`.

### 4.5 Chain share body

A chain share travels with a body a fruit does not need: `peaks` (the MMR accumulator of the
chain shares before it, whose root is `prev_root`), `fruits` (the sorted list of share
hashes whose Merkle root is `fruits_root`) and `txs` (the transactions whose Merkle root is
`txs_root`, doc/sharechain-transfers.md §3). On the wire: `ShareMessage = share ‖ has_body ‖
[peaks ‖ fruits ‖ txs]`.

## 5. The coinbase

The template a participant mines is a normal Freicoin block template except for its coinbase:

```
vin[0]:        height ‖ extranonce (as today)
vout[0..N-1]:  PPLNS payout outputs (§9), canonical order
vout[N]:       commitment output: value 0, scriptPubKey = OP_RETURN <root (32) ‖ COMMIT_ID (4)>
nLockTime, lock_height
```

The commitment output is the **last** output, so the 36 bytes before `nLockTime` are the
commitment, as in the block-final format. The coinbase holds nothing but the payouts and the
commitment (a verifier rebuilds exactly this, §4.3). The root is a Merkle hash map keyed by
`CHAIN_ID`, so a template may carry other commitments in the same slot. The payouts and the commitment are inserted *before* the segwit
commitment is computed, so the resulting block is valid for every node.

Because the aux-pow commits to the whole Freicoin template through the commit Merkle root
(`m_commit_hash_merkle_root`, computed from the coinbase with an empty scriptSig), the parent
header's hash covers the coinbase and therefore the payouts and the share header (§6.1 rule 4).

## 6. Validation

### 6.1 Fruit (`AcceptFruit`)

A share `S` hanging from anchor `A` (a known chain share, or nothing if the chain is empty) is a
valid fruit iff, with the rejection reason in brackets:

1. **Not seen** before [`share-duplicate`].
2. **Linkage**: `A` known [`share-anchor-unknown`], or `anchor` null only while the chain is
   younger than `R` (fruits of the empty-chain template, listable by the first `R` chain shares)
   [`share-anchor-null`]; `height = A.height + 1` (0) [`share-bad-height`];
   `total_work = A.total_work + work(A.bits)` (0) [`share-bad-total-work`];
   `prev_root = MMR(chain shares … A).root` [`share-bad-prev-root`];
   `bits = RequiredBits(A)` (§7.3) [`share-bad-bits`]; `share_chain_path = CHAIN_ID`
   [`share-bad-chain-id`].
3. **Proof of work**: PoW hash (§4.3) ≤ target(`S.bits`), reconstruction not mutated
   [`share-high-hash`]. (`S.bits` is fixed by rule 2, so this is the retargeted share
   target, not the chain's easiest one.)
4. **Aux binding** (aux shares): the commit root recomputed from the witness coinbase (empty
   scriptSig, sequence 0) through `branch` equals `aux.m_commit_hash_merkle_root`
   [`share-aux-commit-mismatch`]. The parent header hash therefore binds the payouts.
5. **Timestamp**: `nTime ≤ now + 2 h` [`share-time-too-new`]; `nTime ≥ A.nTime − 10 min`
   [`share-time-too-old`].
6. **Template age**: `hashPrevBlock` is a known Freicoin block [`share-unknown-prev-block`]; a
   *live* share (just mined, or relayed by a peer) builds on a block no deeper than
   `MAX_TEMPLATE_AGE` below the verifier's tip [`share-stale-template`]. Shares arriving in a
   sync batch or from `sharechain.dat` keep only the known-block requirement. The witness
   `nBits` equals the Freicoin difficulty required on top of `hashPrevBlock`
   [`share-bad-frc-bits`], since it sizes the PPLNS window (§9).
7. **Payout**: `total > 0` and the commitment branch is well formed [`share-bad-coinbase`]; the
   coinbase rebuilt from the witness fields with outputs `PPLNS(A, total, S.miner)` (§9) plus
   the commitment (§4.3) has txid `cb_hash` [`share-bad-payout`]. The check is skipped when
   the window walked back from `A` runs into a trusted base (§6.3): those credits are history
   the node never had (and rule 4 is then skipped too, as it needs the coinbase).

A fruit whose anchor is unknown is parked in a bounded orphan pool and retried when its anchor
arrives (a range sync is requested, §10); one whose `hashPrevBlock` is unknown is parked and
retried when the Freicoin tip changes.

### 6.2 Chain share (`AcceptChainShare`)

A fruit `S` with body (`peaks`, `fruits`) is a valid chain share iff:

1. It is a valid fruit (6.1, recorded first) and not already a chain share
   [`chainshare-duplicate`].
2. **PoW**: PoW hash ≤ target(`S.bits`) / `M` [`chainshare-high-hash`].
3. **Peaks** (C1): `peaks = A.peaks.Append(hash(A))` and `peaks.root = S.prev_root`
   [`chainshare-bad-peaks`].
4. **Fruit list** (C2): `fruits` sorted and unique [`chainshare-fruits-unsorted`]; its Merkle
   root is `S.fruits_root` [`chainshare-bad-fruits-root`]; every listed fruit is known
   [`chainshare-fruit-unknown`], is not listed by any of the last `R` chain shares ending at
   `A` [`chainshare-fruit-relisted`], and its anchor is known [`chainshare-fruit-anchor-unknown`]
   and at height ≥ `S.height − R` [`chainshare-fruit-stale`]. Within `R` heights above a
   trusted base (§6.3) unknown fruits and unknown fruit anchors are tolerated: they hang from
   chain shares the node never had.
5. **Tip** (C3): the chain share with the greatest *verified* work is the tip: `total_work +
   work(bits) − base.total_work`, where `base` is the lowest chain share of its chain (§6.3).
   Ties keep the current tip (first seen). After a restart the tip saved in `sharechain.dat`
   wins among equal-work forks.

### 6.3 Trusted base

A node with no chain of its own (a fresh node syncing from peers, or a node reloading a pruned
`sharechain.dat`) cannot validate the first chain share it receives: its anchor has been
pruned everywhere. It therefore takes that chain share as its **base** (`AcceptBaseChainShare`):
only the share's own consistency is checked (chain id, `bits` not easier than `SHARE_BITS`, PoW
against its own bits, aux binding, `peaks.root = prev_root`, fruit list sorted with the right
root, timestamp not in the future), its `total_work` and `bits` are taken as claims. A base is
accepted only while the chain is empty and only from the first `shares` batch answering our own
`getshrchain` (the one that starts at the peer's base) or from our own file, never from a
relayed `share`. Shares at or below the
base's height with unknown anchors are dropped [`share-below-base`].

Because the base's claim is unverified, forks are compared by the work verified above the
base (rule 5), as p2pool weighs only the shares it has seen. A peer that serves a fake base
with a huge `total_work` gains nothing; to win the tip it must out-work the honest chain over
the depth the node has actually received. The depth a syncing node gets is whatever its peers
still hold (§10 pruning: the window plus `R` plus 64 chain heights), which is what bounds
this trust.

A template builder lists (`ListableFruits`) every known fruit whose anchor is on its chain
within `R` heights and that none of the last `R` chain shares listed; fruits at height 0 are
listable only while the chain is shorter than `R`. Listing does not require the fruit's anchor
to be on the *lister's* branch when verified (rule 4 checks only recency and knowledge), which
is what makes credit independent of chain races (doc/sharechain-uncles.md §4).

## 7. Chain selection and difficulty

### 7.1 Best chain

Most verified work (§6.2 rule 5). Reorganizations are expected and cheap: no state is
attached to a chain share beyond its index entry (header, peaks, fruit list, parent pointer,
the base's claimed work).

### 7.2 Fruits instead of uncles

There is no uncle rule. Off-chain work is credited by being listed (§6.2 rule 4, §9), with no
penalty and no includer reward; `R` is the only parameter. The argument, the comparison with
p2pool, Ethereum ommers, P2Pool-Monero and StrongChain, and the simulation are in
doc/sharechain-uncles.md.

### 7.3 Share target (`RequiredShareBits`)

Regtest: fixed at `SHARE_BITS`. Main: the target for a share hanging from anchor `A` is the
linearly weighted moving average (LWMA) over the last 60 chain shares ending at `A` (newest
weighs most) of their solve time, against the expected chain-share spacing `T = M ×
SHARE_INTERVAL` (60 s), clamped to `[1, 6 T]`, applied to the average target of those shares, then clamped to `[0.75, 1.25] × A.target` and never easier than `SHARE_BITS`.
A chain with fewer than two chain shares uses `SHARE_BITS`. Fruits do not enter the average.
Chain shares arrive every `M × SHARE_INTERVAL` ≈ 60 s on main.

## 8. Bandwidth

At 10 s per share: ≈ 8 MB/day of aux shares for a node that receives every share once,
whatever the number of payees. A `shares` batch carries at most 500 shares; a locator at
most 64 hashes. Per peer: 120 shares/min, 6 locators/min, and at most 4 unsolicited batches
in flight (§10).

## 9. PPLNS

Reference model with vectors: `contrib/sharechain/pplns_fruits.py` →
`src/test/data/sharechain_pplns.json`, checked by `src/test/sharechain_tests.cpp`.

Inputs: the anchor `A`, `TOTAL` (kria, the coinbase's total), the builder's address `B`, the
window work `W = WINDOW_K × work(T)` where `T` is the target the block's miners face on top of
`hashPrevBlock`: the aux target (`GetNextWorkRequiredAux`) once merge mining is active, the
native one before. It is taken from the block chain, never from the share, so template,
verifier and pruning agree. The window spans at most `WINDOW_SHARES` chain shares: for a
pool mining a fraction `p` of the network the work rule alone would hold
`WINDOW_K × 10 min / p` of shares (46 days at 0.06 %, growing as the network grows), which
is state every node must keep and walk on every check; the cap holds it to 3 days, as
p2pool's `CHAIN_LENGTH` does, and only binds for pools below about 1 % of the network. PPLNS
pays each share its work divided by the block's work in expectation whatever the window is,
so the cap changes who a block pays (the last 3 days), not what a share is worth.

1. **Credits** (`ComputeShareCredits`): walk the chain shares backwards from `A`; for each, add
   `work(f.bits)` to `f.miner` for every fruit `f` it lists; stop after the chain share whose
   fruits bring the accumulated work to `W` (counted in full) or after `WINDOW_SHARES` chain
   shares, whichever comes first. Chain shares earn nothing for
   being chain shares (they are listed as fruits by a later chain share like any other).
   Unknown fruits are skipped, so a verifier must hold every listed fruit (§6.2 rule 4
   guarantees it).
2. **Split** (`ComputeSharePayouts`): credits are scaled so their sum fits in 190 bits;
   `out_m = floor(TOTAL × credit_m / Σcredit)`; the flooring remainder goes to `B` (added to an
   existing output, or a new output; omitted if zero). `Σout = TOTAL` exactly.
3. **Dust**: every `out_m < DUST` with `m ≠ B` is removed from `credit`; recompute step 2 once.
4. **Cap**: if more than `MAX_PAYEES` remain, keep the `MAX_PAYEES` largest credits and
   recompute step 2 once.
5. **Order**: outputs sorted by (witness version, program) ascending. The verifier builds the
   coinbase from this vector and compares its txid with `cb_hash`.

With an empty chain the builder receives `TOTAL`. The window slides with shares, not with
blocks: a found Freicoin block does not reset it.

## 10. Node integration (freicoind)

- **Enable**: `-stratum` plus `-sharechain=main|regtest` (`solo` = skeleton default, no chain).
  Payout address `-defaultshareaddress` (defaults to `-defaultminingaddress`; must be a witness
  address) or the stratum username address.
- **Template**: `GetWorkUnit` snapshots the share chain tip (anchor, `prev_root`, listable
  fruits, bits, credits) into the work unit; `CustomizeWork` builds the payout vector for the
  client's address and appends the commitment before the segwit commitment is computed.
  Per-address commits in `mining.aux.notify` carry the share header for that address.
- **Share target to miners**: native `mining.set_difficulty` and the `bits` of
  `mining.aux.notify` carry the share target, so mergemine bitcoind submits every parent hash
  that meets it (`mining.aux.submit` with the full `AuxProofOfWork`). No mergemine change.
- **Submit path**: `mining.submit` and `mining.aux.submit` reconstruct the share from the work
  unit and the submission, run §6.1 (and §6.2 when the PoW meets the chain target), relay, and
  continue into the usual block path if the Freicoin target is met. Second-stage submissions of
  a merge-mined block earn no share (the first stage already did).
- **Work refresh**: when the share chain tip changes (local or relayed chain share) every
  connected miner gets a new job, so fruits hang from the newest anchor.
- **P2P**: service bit `NODE_SHARECHAIN` (1 << 12), set when the share chain is enabled.
  Messages: `share` (one `ShareMessage`), `getshares` (hashes), `getshrchain` (locator of up to
  64 chain-share hashes, newest first), `shares` (batch of up to 500 `ShareMessage`s, oldest
  first, including forked chain shares and fruits not yet listed). A node asks `getshrchain`
  from every `NODE_SHARECHAIN` peer at `verack` and whenever a relayed share's anchor is
  unknown. While its chain is empty, the node accepts the first chain share of a batch as its
  base (§6.3). Rate limits per peer as in §8; a rejected share is logged, not penalized, since
  a peer may simply be behind.
- **Persistence**: `sharechain.dat` (version 4: tip hash, then every chain share with body and
  every fruit, orphans last) written at shutdown, loaded after block headers are available.
- **Pruning** (`Prune`): keep chain shares from the window floor (work or `WINDOW_SHARES`,
  whichever ends it first) minus `R` minus 64 heights;
  drop fruits whose anchor is gone. The lowest kept chain share becomes the node's base (§6.3)
  and is where `getshrchain` answers start. `getsharechaininfo.baseheight` reports it.
- **RPC**: `getsharechaininfo` → `enabled, network, sharebits` (initial target), `chainfactor,
  recency, height, tip, chainshares, fruits, baseheight, nextbits, nextdifficulty` (the
  retargeted share target the next share must meet); `getsharecredits [amount]` (Vintage
  Hash) → the window as the template sees it: size and fill (`windowbits, windowk,
  windowshares, windowwork, filledwork, filled, tipheight, lowheight, chainshares,
  fruits, pending`) and per address `fruits, work, share, payout` for a block found now;
  `transfershares`, `claimshares`, `listsharetxs` (doc/sharechain-transfers.md §7).

## 11. Security considerations

- **Payout tampering**: a template paying anything other than `PPLNS(A)` is not a valid share;
  if it becomes a Freicoin block, that is ordinary solo mining outside the chain.
- **Aux binding**: an aux share is accepted only if the parent header hash commits (through the
  commit root) to the coinbase in the witness, so a miner cannot present one coinbase to the
  Bitcoin network and another to the share chain.
- **Block withholding**: a participant who withholds a found block loses their own share of it
  and gains nothing; withholding shares only reduces their own credit.
- **Chain races and selfish mining**: a chain share that loses a race still lists its fruits,
  and those fruits stay listable for `R` heights, so neither the loser nor the fruits' miners
  lose pay (FruitChains fairness); withholding chain shares cannot steal credit either.
- **Stale-template farming**: rule §6.1.6 bounds how old a template may be.
- **Spam**: shares cost real hashes at or above `SHARE_BITS`; per-peer rate limits and a bounded
  orphan pool cap what a peer can make a node store or verify.
- **Bootstrap**: a fresh node trusts the first chain share its peers serve (§6.3). It is
  exposed to a peer feeding it a fabricated chain only up to the work above that base, and only
  until it has peers with the honest chain; comparing by verified work makes the attack cost
  real hashes over the whole served depth. Connecting a fresh node to known participants first
  removes the exposure.
- **Reorg of the Freicoin chain**: shares reference a Freicoin `hashPrevBlock`; after a reorg
  shares on the orphaned branch remain valid shares and keep their credit.
- **Sybil payees**: splitting hashrate across addresses only adds outputs; `MAX_PAYEES` and
  `DUST` bound the cost.
- **Window size from the template's `nBits`**: the window is sized by the `nBits` in the
  witness; rule §6.1.6 requires it to be the Freicoin difficulty at `hashPrevBlock`, so a miner
  cannot shrink the window by claiming an easier Freicoin target. `WINDOW_SHARES` bounds it
  from above, so a small pool's nodes cannot be made to hold and walk months of shares.

## 12. Open questions

1. `WINDOW_K` (3–5), and whether to size the window by Freicoin-block work (as here) or by a
   fixed share count as p2pool does. **Settled 2026-10-29: both, the smaller wins (§9).
   Live at 0.06 % of the network the work rule gave a 46-day window; `WINDOW_SHARES` = 4320
   caps it at 3 days of chain shares. `WINDOW_K` stays 4.**
2. `M` and `R` on main: `M = 6`, `R = 10` give a 60 s chain-share interval and a 10-minute
   listing window; the simulation in doc/sharechain-uncles.md used `R = 5–10`.
3. `MAX_TEMPLATE_AGE = 2` and the timestamp slack (`−10 min / +2 h`).
4. (Settled by the first live run: the window is sized by the aux target, see §9.)
5. Initial `SHARE_BITS` (difficulty 10 000 ≈ 43 TH/s for 10 s shares) and the LWMA
   parameters, to be tuned on real hashrate.
6. Bootstrap trust (§6.3): whether the first served chain share is an acceptable base, or
   whether fresh nodes should require the same base from several peers, or a configured
   checkpoint.
7. `MAX_PAYEES` at scale (raised by Vintage Hash): with more miners in the window than the cap
   allows, the smallest never reach a coinbase and their fruits expire, the problem the chain is
   meant to fix. `DUST` does not bite at any realistic size (1000 kria against a ~95 FRC reward
   is a 10⁻⁷ share of work). The cap is bound by share size, not block size: every share
   carries the full coinbase, 1 000 payees ≈ 31 KB per share ≈ 270 MB/day. Options: rotate
   the cap deterministically (top `N` always, the rest in turn by block height; expectation
   kept, variance up), or carry unpaid credit forward, which is state every node must compute
   alike and therefore a validation rule; or shares that commit to the payout vector instead
   of carrying it. **Settled 2026-10-28 (with Vintage Hash): shares commit to the payout
   vector (`cb_hash`, §4.2), `MAX_PAYEES` raised to 1000, bounded by the block.**
8. Coinbase outputs per miner and small miners' income (raised by M. Friedenbach, 2026-10-29):
   transferable share credits sold to market makers for lightning payments, see
   doc/sharechain-transfers.md (design proposal).

## 13. Deliberate exclusions

The skeleton's `SOLO` type, light share-chain clients and proofs, a share-chain explorer and
statistics, cross-chain (multi-aux) share chains, and payout outputs other than witness
programs are out of scope for the first implementation.

## 14. Implementation status (branch `sharechain`, 2026-10-23)

Everything in §4–§10 is implemented in freicoind on top of 28.1 and the `tmp` skeleton:

- `src/sharechain.{h,cpp}`: share and witness formats, `GetShareWork`, `ComputeShareCredits`,
  `ComputeSharePayouts` (unit vectors from `contrib/sharechain/pplns_fruits.py` in
  `src/test/sharechain_tests.cpp`), `ShareChainstateManager` (fruit and chain-share
  acceptance, listable fruits, template state, LWMA retarget, orphan pool, range sync,
  pruning, trusted base and verified-work tip selection, `sharechain.dat` v4 with the saved
  tip), share transactions and the credit ledger (`ShareTx`, `ShareLedger`,
  `ComputeWindowCredits`, the transaction pool), `getsharechaininfo`, `transfershares`,
  `claimshares`, `listsharetxs`; `getsharecredits` (Vintage Hash) in `src/stratum.cpp`;
- `src/stratum.cpp`: payouts + commitment in the coinbase before the segwit commitment, share
  target to native miners and in `mining.aux.notify`, acceptance on `mining.submit` and
  `mining.aux.submit`, no credit for second-stage submissions, work refresh on tip change
  (pushed from the event loop thread: BlockWatcher wrote to the clients' bufferevents from its
  own thread, which crashed inside libevent once pushes became frequent), `-miningmindiff`
  fixed to match its registered name; miner compatibility fixes from Vintage Hash's live
  setup (optional `min-bit-count` in `mining.configure` for ESP-Miner, up to four
  `mining.subscribe` parameters for Braiins OS, work re-sent after 60 s idle);
- `src/net_processing.{h,cpp}`, `src/protocol.h`: `NODE_SHARECHAIN`, `share`, `getshares`,
  `getshrchain`, `shares`, `sharetx`, relay, sync at `verack` and on unknown anchor, per-peer
  limits, orphan retry on Freicoin tip change;
- `src/init.cpp`, `src/common/args.cpp`: `-sharechain=<name>`, `-defaultshareaddress`, dump at
  shutdown, load before the network starts.

Unit tests (`src/test/sharechain_tests.cpp`): PPLNS vectors and edge cases, acceptance and
payouts of fruits and chain shares, every rejection reason of §6, prune of a 90-share chain
followed by a fresh peer syncing it from the base and a reload of the pruned dump, verified
work, the `WINDOW_SHARES` cap of the window, transfers of credits (unconditional,
hash-locked and claimed, lapsed, double sell, replay, malformed, reload), LWMA direction and
clamps. Verified on regtest (`contrib/sharechain/regtest*`, Docker): single node with two miners,
merge mining through mergemine bitcoind (aux fruits and chain shares, Freicoin blocks found),
two nodes with relay and restart. `test/functional/feature_sharechain.py` (in
`test_runner.py`) covers two mining nodes converging on one share chain, coinbase payouts and
commitment, a late third node syncing with `getshrchain`, reload from `sharechain.dat`, and
propagation after the restart.

Not done: §12 items, mainnet parameters on real hashrate, the §13 exclusions.
