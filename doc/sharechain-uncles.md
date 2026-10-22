# Crediting off-chain work in a share chain: the minimal algorithm

```
Status:  Research note for doc/sharechain.md (replaces its §7.2 "Uncles" if accepted)
Author:  Kirill Denisov (denisov-k)
Date:    2026-10-22
Question (M. Friedenbach, 2026-10-22): classic p2pool loses too many shares to orphaning, which
         ruins returns for small miners; some uncle-like credit for off-chain work is needed; the
         known protocols are complex and unproven. What is the minimal, small, easy-to-reason-about,
         verified-correct algorithm, even if not the most efficient?
```

## 1. Summary

Replace "uncles" with **fruits** (Pass & Shi, FruitChains, PODC 2017): a share does not have to be
*on* the chain to be paid, it only has to be *recorded by* the chain within a bounded number of
chain steps after the point it hangs from. Three rules, no penalty, no includer reward, no
"same-branch" test, one parameter `R`:

1. Every share hangs from a chain share (its *anchor*) and must meet the share target.
2. A chain share is a share that also meets the chain target (one hash attempt serves both). It
   lists every valid share it knows whose anchor is at most `R` chain heights behind it and that
   no chain share in its last `R` ancestors already lists.
3. Credit for PPLNS = the full work of every share listed by the chain within the window. Chain
   shares themselves earn nothing extra.

Under a bounded network delay and an honest majority of chain shares, every honest share is
listed with overwhelming probability, so credit equals work regardless of latency: this is the
FruitChains fairness theorem, and it is the only property we need. A 72-hour simulation with six
miners and delays from 0.3 s to 8 s at a 10 s share interval gives credited/earned = 1.000 for
every miner, against 0.44–0.90 for classic p2pool and 0.80–1.01 for Monero-style uncles (§6).

## 2. Why uncles are the wrong primitive

An *uncle* rule asks: "is this orphaned share close enough to the chain to deserve partial
credit?" That question drags in a branch test, a depth limit, a penalty schedule and an
includer incentive, and every one of those is a knob with no principled value. The fruit rule
asks a different question: "did the chain *see* this share in time?" Seeing is cheap, has no
competitor, and needs no penalty because a listed share does not displace anything.

The orphaning that hurts in classic p2pool comes from one design choice: the share that earns
credit and the share that orders the chain are the same object, so a propagation race on the
chain is a race for pay. Separating the two (2-for-1 mining: every hash attempt is a share, one
in `M` is also a chain share) makes chain races pay-neutral. The chain can orphan freely; the
pay record cannot.

## 3. What exists, and what each one proves

| Scheme | Rule for off-chain work | Parameters | What is shown | Complexity |
|---|---|---|---|---|
| Classic p2pool (2011) | none: orphaned share = 0 | share interval | nothing; measured orphan+DOA losses of several % at 30 s shares, worse for slow nodes | small |
| Ethereum PoW ommers (2015–2022) | orphan paid `(8−d)/8` of a block reward for depth `d ≤ 6`, includer gets `1/32` per ommer, ≤ 2 per block | depth, two schedules | heuristic; known uncle-farming and selfish-mining interactions | medium |
| P2Pool for Monero (2021) | orphan within depth 3 on the same branch, 80 % to its miner, 20 % to the includer, hashes sorted, cap per block | depth, penalty, cap | engineering; no proof; works in production | medium |
| StrongChain (USENIX Sec 2019) | *weak headers* (PoW between weak and strong target) pointing to the **last** strong header are listed by the next strong block and paid pro rata | weak target | analytical and simulation arguments for lower variance and selfish-mining resistance; weak headers that miss the next strong block are lost (recency 1) | small |
| FruitChains (PODC 2017) | *fruits* hang from a block at most `R` back; any later block records unrecorded recent fruits; reward split over recorded fruits | `R`, fruit/block hardness | **theorem**: honest parties receive a fraction of recorded fruits within `(1±δ)` of their relative power, for adversary `< ½` and bounded delay; selfish mining gains nothing because withheld blocks' fruits are recorded later | small |

StrongChain is FruitChains with `R = 1`; Monero's uncles are fruits restricted to one branch,
one depth window and a penalty. FruitChains is the general, proven form, and it is also the
smallest to state. Note that FruitChains is a *consensus* protocol (fruits carry transactions);
here we only borrow its reward layer, which is the part the theorem is about. The known critique
of FruitChains (Kiayias et al. 2023, "On the (De)centralization of FruitChains") concerns pooling
to share template-construction cost, which does not apply to a share chain where every
participant already runs a node by assumption.

## 4. The algorithm, stated for the Freicoin share chain

Objects (names as in `doc/sharechain.md`):

- **share** `S`: ShareHeader + ShareWitness as specified, with `prev_shares` replaced by three
  fields: `anchor` = hash of the chain share it hangs from, `prev_root` = MMR root of the chain
  ancestors of `anchor` plus `anchor` itself, and `fruits_root` = Merkle root of the sorted list
  of share hashes the template records (empty list allowed). All three are in the hashed header
  because with 2-for-1 mining the template is hashed before anyone knows whether it will turn out
  to be a chain share. PoW: Bitcoin-header hash ≤ `target(S.bits)`.
- **chain share** `C`: a share whose PoW hash also ≤ `target(C.bits) / M`. It is transmitted with
  the body behind its roots: the MMR peaks and the `fruits` list. A plain share carries the roots
  only (its list is never used, so it is never sent).

Rules:

```
A1  anchor(S) is a chain share known to the verifier.
A2  S.outputs == PPLNS(anchor(S))                          -- payouts are fixed by the anchor
C1  anchor(C) == parent(C); height(C) == height(parent)+1; prev_root(C) == root(MMR(parent) ⊕ hash(parent));
    fruits_root(C) == merkle(C.fruits)
C2  for every f in C.fruits:
        f is a valid share
        height(C) - height(anchor(f)) <= R                 -- recency
        no chain share among the last R ancestors of C lists f   -- single listing
    C.fruits is sorted, without duplicates
C3  chain selection: greatest chain work (sum of chain-share work), ties first-seen
P   credit(m) = sum of work(f) over fruits f listed by chain shares in the PPLNS window whose miner is m;
    payouts from credit as in doc/sharechain.md §9 (floor split, remainder to builder, dust, cap, canonical order)
```

That is the whole thing. `M` sets how often chain shares occur (chain target = share target / M);
`R` sets how long a share stays listable. No penalty, no depth-dependent schedule, no includer
reward, no branch test.

### 4.1 Why the MMR moves

Mark's skeleton puts `prev_shares` in every share so a share proves its ancestry. With fruits,
only chain shares need ancestry; a plain share needs one pointer (`anchor`). The hashed share
header stays fixed-size: `anchor`, `prev_root` and `fruits_root` (96 B) replace the MMR root
(32 B). Only the bodies behind the two roots differ between a plain share (never transmitted)
and a chain share (MMR peaks and the fruit list). Keeping the MMR in chain shares preserves what
the skeleton wanted it for: compact proofs that a share is in history, which §P never needs but a
light client might.

### 4.2 What a verifier checks on receipt

- share: A1, A2, PoW, timestamp, uniqueness. O(1) plus one PPLNS evaluation at the anchor
  (memoized per anchor: every share hanging from the same anchor has the same payout vector).
- chain share: everything above plus C1, C2 (a walk of `R` ancestors with a set lookup per
  listed fruit), C3. O(R + |fruits|).

## 5. Properties and why they hold

Let `Δ` be the maximum one-way propagation delay, `T_c = M·T_s` the mean chain-share spacing,
`α < ½` the adversarial fraction of chain-share power.

**P1. Single credit.** A fruit is listed at most once on any chain. *Proof.* C2 forbids listing a
fruit already listed in the last `R` ancestors, and C2's recency bound makes a fruit unlistable
beyond `R` heights after its anchor, so any two listings would be within `R` of each other,
contradicting the first clause. The verifier's walk is bounded by `R` for the same reason.

**P2. Credit = work (fairness).** An honest fruit `f` mined at time `t` is listed with
probability ≥ `1 − α^k` where `k = R − ⌈2Δ/T_c⌉ − 1`. *Sketch.* `f`'s anchor is at most `Δ` old,
so at most `⌈Δ/T_c⌉` chain heights are consumed before `f` is mined; by `t + Δ` every honest
miner has `f`; from then on every honest chain share lists `f` (C2 says it must, and listing is
free) until the recency bound, which leaves at least `k` chain heights in which the first honest
chain share records it. Each height is adversarial with probability `α`. With `α = ⅓`, `Δ = 8 s`,
`T_c = 60 s`, `R = 10`: `k = 8`, loss probability `≈ 1.5·10⁻⁴`. This is the FruitChains
"fruit freshness" lemma specialized to our parameters; the theorem in Pass & Shi gives the
`(1±δ)` bound over any long enough window as a consequence.

**P3. Chain races are pay-neutral.** Orphaning a chain share changes nothing in `P` except
*which* chain share lists the fruits, because listed sets are determined by anchors and the
recency bound, not by who wins the race; the orphan's own fruit credit is unaffected (it is a
share like any other and gets listed by the winner). Hence selfish mining of the share chain has
no reward to capture. *Proof.* Immediate from `P` not referencing chain-share identity.

**P4. Omission does not pay.** A chain-share miner who omits someone else's fruit raises its own
PPLNS fraction by at most `work(f)/W` per omitted fruit for the brief time until an honest chain
share lists it (P2), after which the omission is undone. Expected gain `≤ α·work(f)/W` per fruit,
which is below any reasonable cost of deviating from reference software. This is the reason no
includer reward is needed: inclusion is free and omission is self-defeating under P2.

**P5. Bounded state.** A verifier keeps chain shares back to the PPLNS window plus `R`, and
fruits listed by them. Per chain share: `|fruits| ≈ M` on average. Nothing else accumulates.

**P6. Stale-anchor mining does not pay.** Hanging from an old anchor (within `R`) yields the same
credit as hanging from the tip and only risks expiry; hanging from an anchor older than `R` is
invalid. There is no analogue of uncle farming because there is no depth-dependent reward.

What is *not* claimed: P2 assumes honest chain-share majority and bounded delay, as every PoW
result does; a 51 % adversary can censor fruits exactly as it can censor transactions.

## 6. Simulation

`contrib/sharechain/orphan_sim.py`, 72 simulated hours, Poisson shares at `T_s = 10 s`, six
miners with hashrate shares 40/25/15/10/6/4 % and one-way delays 0.3/0.5/1/2/4/8 s, first-seen
chain tip. Rules: **A** classic (credit only on the main chain), **B** Monero-style uncles
(depth 3, 80 % to the orphan's miner, 20 % to the includer), **C** fruits (`M = 6`, `R = 10`).

| miner | hashrate | delay | A classic | B uncles | C fruits |
|---|---|---|---|---|---|
| 0 | 40 % | 0.3 s | 0.905 | 1.011 | 1.000 |
| 1 | 25 % | 0.5 s | 0.890 | 1.008 | 1.000 |
| 2 | 15 % | 1.0 s | 0.855 | 1.000 | 1.000 |
| 3 | 10 % | 2.0 s | 0.779 | 0.982 | 1.000 |
| 4 | 6 % | 4.0 s | 0.640 | 0.938 | 1.000 |
| 5 | 4 % | 8.0 s | 0.441 | 0.803 | 1.000 |
| work lost overall | | | 15.4 % | 0.7 % | 0.0 % |

Reading: classic p2pool taxes latency hard, exactly Mark's objection. Uncles recover the lost
work but *redistribute* it: the two fastest miners end above 1.0 and the slowest at 0.80, because
the 20 % penalty flows from slow to fast miners. Fruits credit everyone exactly. Sensitivity:
`R = 4` still loses 0.01 %; `R = 2` loses 0.22 % concentrated on the slowest miner; the strict
anchor rule (§9.1) loses 0.01 %; `M = 1`
(every share is a chain share, `R = 10`) also gives 1.000 for all, which shows the gain comes
from the "listed within R by anyone" rule, not from the two-tier structure. `M > 1` only reduces
chain churn and verifier work.

The simulation assumes honest listing by all chain shares; P4 is the argument that this is the
rational behaviour, not a modelled result.

## 7. Parameters

| Parameter | Proposed | Reasoning |
|---|---|---|
| `T_s` (share interval) | 10 s | as in the spec; 15 s if traffic matters |
| `M` (chain share every M shares) | 6 | chain share per minute: enough to bound verifier walks, few enough that `R` heights span minutes |
| `R` (recency, chain heights) | 10 | ≈ 10 min of listing opportunity; P2 loss `< 10⁻³` even at `α = ⅓`, `Δ = 8 s` |
| `W` (PPLNS window) | as spec §9, measured in fruit work | unchanged |
| uncle depth / penalty / includer reward | removed | §2, §5 |

## 8. Changes to `doc/sharechain.md` if adopted

- §4.1: `prev_shares` → `anchor ‖ prev_root ‖ fruits_root` in ShareHeader (all hashed); chain
  shares additionally transmit `ChainShareBody = MMR peaks ‖ fruits`, flagged in `version`.
- §6: validation rules A1–A2 for shares, C1–C3 for chain shares (this note §4).
- §7.2: replaced by this note; §7.3 unchanged (share target); chain target = share target / `M`.
- §9: credit over listed fruits; payout vector of a share is `PPLNS(anchor)`.
- §10: `mining.aux.notify` carries the share target; freicoind tests both targets on submit.
- §12: questions 2 and 5 close; question 1 (`WINDOW_K`) and 3 (template age) remain.

## 9. Open points

1. Fruits hanging from an orphaned chain share. Two options were simulated: a one-level
   fallback (the fruit is credited if the orphan's parent is on the chain within `R`) and the
   strict rule (the anchor itself must be on the recorder's chain). Strict loses 0.01 % at
   `M = 6` and 0.07 % at `M = 1`, indistinguishable from the fallback, so **the strict rule is
   proposed**: it keeps every payout vector anchored to a chain share on the recorder's own chain
   and needs no special case.
2. Whether `anchor` should be the chain share hash or its MMR root (the latter keeps Mark's
   fixed-size-header intent and makes a share's payout vector verifiable from the MMR alone).
3. `R` and `M` under a global miner set with 2–3 s delays (the 8 s case above is deliberately
   pessimistic).

## References

- R. Pass, E. Shi, "FruitChains: A Fair Blockchain", PODC 2017; ePrint 2016/916.
- P. Szalachowski, D. Reijsbergen, I. Homoliak, S. Sun, "StrongChain: Transparent and
  Collaborative Proof-of-Work Consensus", USENIX Security 2019.
- A.-P. Kiayias et al., "On the (De)centralization of FruitChains", arXiv 2307.12422, 2023.
- SChernykh, P2Pool for Monero, `src/side_chain.cpp` (uncle depth 3, penalty 20 % credited to the
  includer, sorted unique uncle hashes).
- Ethereum PoW ommer rules: `(8−d)/8` for `d ≤ 6`, `1/32` per ommer to the includer.
- Classic p2pool: 30 s share chain, payouts over recent shares (bitcoin.it/wiki/P2Pool).
