# Light fruits: small miners under a large one

Status: proposal for review, not implemented. Builds on doc/sharechain.md (§6.1, §7.3, §9).

## 1. Problem

Fruits are credited by their work, and every fruit must meet the share target, which the
LWMA keeps at one fruit per `SHARE_INTERVAL` (10 s) for the whole share chain (§7.3). The
target follows the largest participant. Today the chain runs at ~20 TH/s and a 1 TH/s Bitaxe
finds a fruit every few minutes. If a 25 PH/s miner joined:

| | today | with 25 PH/s |
|---|---|---|
| share difficulty | ~5·10⁴ | ~6·10⁷ |
| PPLNS window (4 blocks of work) | 3-day cap | ~1 hour |
| fruits of a 1 TH/s miner | every ~4 min | every ~3 days |
| fruits of that miner per window | hundreds | 0.02 |

Expected income is unchanged, but it arrives as one fruit worth ~1 FRC every three days, and
a fruit pays only while it is in the window. This is the variance that drove small miners out
of p2pool, and selling credits cannot help a miner who has no fruit to sell.

## 2. Options

1. **Shorter `SHARE_INTERVAL`.** 1 s instead of 10 s is 10× more fruits for everyone and 10×
   the relay load; a Bitaxe would still wait ~7 hours. Not enough.
2. **A second, lighter share chain** (p2pool-mini on Monero). It pays only from blocks it
   finds at its own hashrate. With one miner at 99.99 % of the network, a chain of small
   miners finds a block every few months: the problem moved, not solved.
3. **Fruits lighter than the share target, credited by their own work** (below). The large
   miner's work and the chain's pace are untouched; only what counts as a fruit widens.

## 3. Proposal: a difficulty ladder for fruits

Let `B = RequiredBits(A)` be today's share target for anchor `A`. A share header's `bits`
becomes the share's own claimed difficulty, and:

1. **Fruit target.** `target(S.bits)` is a power-of-two multiple of `target(B)`, from 1 to
   `2^K` (`K = 10` on main): `S.bits ∈ {B, B·2, …, B·2^K}` in target terms
   [`share-bad-bits`]. PoW ≤ `target(S.bits)` [`share-high-hash`], as now.
2. **Credit.** A fruit is credited `work(S.bits)` (unchanged formula: it already reads the
   fruit's own bits). The expected credit per hash is the same at every rung, so the choice
   is about variance only.
3. **Chain shares.** A share is a chain share iff PoW ≤ `target(B) / M`, whatever its own
   bits; chain work, `total_work` and the LWMA use `B`, not `S.bits`. The chain's pace and
   its security do not depend on how light fruits are.
4. **Listing cap.** A chain share lists at most `MAX_FRUITS` fruits (`500` on main, against
   ~6 per chain share today). Which fruits a builder lists is policy (§5), not consensus.

Nothing else changes: the window, PPLNS, the ledger and transfers read fruits' work as now.

## 4. Stratum

The node sets each miner's fruit difficulty like vardiff, aiming at one fruit per
`FRUIT_GOAL` (5 minutes) for that miner, clamped to the ladder: a 25 PH/s miner stays at `B`,
a 1 TH/s miner drops to the floor `B·2^K`. `mining.set_difficulty` carries the miner's rung;
the share header carries it as `bits`. Merge-mined work (`mining.aux.notify`) gets the same
per-miner rung. With `K = 10` and the table above, the Bitaxe finds a fruit every ~4 minutes
again, ~15 per window.

## 5. Load and abuse

- **Relay.** The rate of light fruits is about the number of small miners divided by
  `FRUIT_GOAL`: 2 500 miners make ~8 fruits a second, 500 per chain share, a few kB/s. The
  cap bounds it; per-peer relay limits (§8) apply per fruit as now.
- **Spam costs work.** Every light fruit carries `work(B·2^K)`; filling the cap with floor
  fruits takes `MAX_FRUITS · work(B)/2^K` per minute, about 2 PH/s at the numbers above.
  Only a large miner can afford it, and it gains no credit by it (credit is work either way).
- **Crowding.** A large miner could still push small miners' fruits off the list. Builder
  policy: list first the fruits of addresses with the fewest fruits in the last `R` chain
  shares, then the oldest anchor, then by hash. Honest builders favour small miners; a
  hostile one only controls the chain shares it finds, and an unlisted fruit stays listable
  for `R` chain shares.
- **Coinbase.** More small payees per window; `MAX_PAYEES`, the dust rule and market makers
  (doc/sharechain-transfers.md) bound the coinbase as before.

## 6. Cost to the code

A new chain id. Rule 2 and rule 3 of §6.1 change (fruit bits on the ladder), §6.2's chain-share
test and `total_work` read `B`, `AcceptChainShare` enforces `MAX_FRUITS`, `ListableFruits`
orders by the policy above, stratum gets per-client fruit difficulty. Unit tests: ladder
bounds, credit by own bits, a light fruit that meets the chain target, the cap.

## 7. Questions for review

1. `K` (the depth of the ladder) and `MAX_FRUITS`: the two numbers trade a small miner's
   variance against relay load.
2. Whether the crowding policy should be consensus (a deterministic order) rather than policy.
3. Whether to tie the ladder floor to an absolute difficulty instead of `B`, so it does not
   rise with the large miner at all (simpler for miners, but the relay bound then depends on
   the chain's hashrate).
