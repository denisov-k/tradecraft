# Freicoin ShareChain: a p2pool-style decentralized payout for solo merge-miners

```
Status:  Draft for discussion — not implemented beyond the data-type skeleton in
         tradecraft:tmp (src/sharechain.{h,cpp}, M. Friedenbach, May–Nov 2023)
Layer:   Node / mining (no consensus change: a share is a regular Freicoin block template;
         payouts are ordinary coinbase outputs)
Author:  Kirill Denisov (denisov-k)
Based on: the ShareChain skeleton above; mergemine v29.0.0.1 stratum/aux-pow protocol;
          p2pool (Forrest Voight, 2011) and P2Pool for Monero (SChernykh, 2021) for the
          share-chain, uncle and PPLNS mechanics
Date:    2026-10-22
```

## 1. Abstract

A **share chain** lets independent merge-miners pool their Freicoin rewards without a pool
operator. Each participant runs the ordinary self-hosted stack (freicoind + mergemine
bitcoind + their own SHA256 hardware). Their freicoind builds Freicoin block templates whose
coinbase already pays every recent contributor in proportion to work (PPLNS). A **share** is
such a template proven by a Bitcoin-header hash that meets a share target far below the
Freicoin target. Shares are gossiped and validated by all participants and linked into a
chain; when a share also meets the Freicoin target it is simply a Freicoin block, and the
payout has already happened inside its coinbase. Nobody holds funds, nobody can redirect them,
and a participant who leaves keeps whatever the window still owes them.

This document fixes the share format, validation rules, chain selection, uncle policy, share
difficulty adjustment and the PPLNS payout function, with test vectors, so that an
implementation inside freicoind can be written and checked against it.

## 2. Design principles

1. **Reuse the aux-pow machinery.** The share commitment travels in the Freicoin coinbase
   exactly like the block-final commitment, and the proof that a template was mined is the
   existing `AuxProofOfWork`. bitcoind/mergemine needs no change.
2. **Shares are self-contained.** A share carries everything needed to verify it except the
   share-chain ancestry it references, so a verifier never needs the sender's mempool or
   template.
3. **Payout is the coinbase.** No pool wallet, no payout transactions, no debt. The coinbase
   of a found block is the only payout mechanism.
4. **Verify proportions, not totals.** The total paid is read from the share itself; the rule
   only constrains how it is split. Fees of someone else's template are irrelevant.
5. **Same binary for solo and pooled mining.** A chain with one participant pays that
   participant everything; joining the main share chain is a configuration switch.
6. **No consensus change.** A node that ignores share chains validates the resulting blocks
   as usual.

## 3. Notation and constants

| Name | Value | Meaning |
|---|---|---|
| `SHARE_CHAIN_ID` | `share_chain_path` (uint256, fixed per share chain) | key of the share commitment in the coinbase Merkle hash map |
| `COMMIT_ID` | `48 49 4a 4b` | 4-byte tag after the commitment root in the coinbase (from the skeleton) |
| `SHARE_INTERVAL` | 10 s (proposed; 15 s acceptable) | target spacing of shares on the chain |
| `WINDOW_K` | 4 (open) | PPLNS window = `WINDOW_K` × expected pool work per Freicoin block |
| `UNCLE_DEPTH` | 3 | maximum height distance between an uncle's parent and the including share |
| `UNCLE_PENALTY` | 20 % | work discount applied to an uncle |
| `MAX_PAYEES` | 100 | maximum payout outputs in a coinbase |
| `DUST` | 1000 kria | outputs below this are dropped and redistributed |
| `MIN_SHARE_TARGET` | per chain parameters | ceiling on share target (floor on difficulty) against spam |
| `work(target)` | `2^256 / (target + 1)` | work of a share, same as block chainwork |

All integers are little-endian unless stated. `VARINT` is the Bitcoin Core `VARINT` (Pieter
Wuille's encoding), `CompactSize` the P2P vector-length encoding.

## 4. Share format

A share is `ShareHeader ‖ ShareWitness`. The header is what the chain links; the witness
proves the header was committed in a mined template.

### 4.1 ShareHeader (from the skeleton, unchanged)

| Field | Type | Bytes | Meaning |
|---|---|---|---|
| `version` | uint32 | 4 | share format version; bit 0 set = aux witness (§4.3), clear = native witness (§4.2) |
| `bits` | uint32 | 4 | compact share target this share had to meet |
| `height` | uint32 | 4 | height in the share chain; genesis share = 0 |
| `total_work` | uint256 | 32 | sum of `work()` of all ancestors (not including this share) |
| `prev_shares` | MMR | VARINT leaf count + 32·peaks | Merkle Mountain Range accumulator of all ancestor share hashes |
| `miner` | witness program | CompactSize version + program | payout address of this share's miner |

**Share hash** = SHA256d over `version ‖ bits ‖ height ‖ total_work ‖ MMR_root(prev_shares) ‖
CompactSize(miner.version) ‖ miner.program`. Only the MMR root enters the hash, so the hashed
header is fixed-size (116 bytes for a 20-byte program).

### 4.2 ShareWitness, native variant (from the skeleton, unchanged)

Used when the share was mined directly against freicoind's stratum (native Freicoin PoW).

| Field | Type | Meaning |
|---|---|---|
| `commit` | VARINT n, then n × (uint8 skip bits, uint256 skip hash) | Merkle-hash-map branch from the share hash to the commitment root, key `SHARE_CHAIN_ID` |
| `cb1` | VARINT len + bytes | the Freicoin coinbase serialized up to the commitment root |
| `nLockTime` | uint32 | the coinbase's lock time (the only field after the commitment) |
| `branch` | VARINT n + n × uint256 | Merkle path from the coinbase (leaf 0) to the Freicoin block's Merkle root |
| `nVersion, hashPrevBlock, share_chain_path, nTime, nBits, nNonce` | 4, 32, 32, 4, 4, 4 | Freicoin header fields; `hashMerkleRoot` is derived, `share_chain_path` sits in its slot |

### 4.3 ShareWitness, aux variant (new)

Used when the share was merge-mined (the common case). The five header fields of §4.2 are
replaced by the Freicoin header fields plus the existing `AuxProofOfWork`:

| Field | Type | Meaning |
|---|---|---|
| `commit`, `cb1`, `nLockTime`, `branch`, `share_chain_path` | as §4.2 | |
| `frc_nVersion, frc_hashPrevBlock, frc_nTime, frc_nBits, frc_nNonce` | 4, 32, 4, 4, 4 | Freicoin header fields (Merkle root derived) |
| `aux_pow` | `AuxProofOfWork` (existing serialization) | commit fields (48), secret (16), commit branch, midstate hash + buffer + length, aux lock time, aux branch, aux txn count, Bitcoin header fields |

Size: ≈ 1.2 KB + 31 bytes per payout output (§8 sizing). With 50 payees ≈ 2.8 KB.

### 4.4 Reconstruction

`GetBlockHeader(share)`:

1. `h0` = share hash (§4.1).
2. `root` = `ComputeMerkleMapRootFromBranch(h0, commit, share_chain_path)`.
3. `cbhash` = SHA256d(`cb1 ‖ root ‖ COMMIT_ID ‖ nLockTime`).
4. `merkle` = `ComputeMerkleRootFromBranch(cbhash, branch, 0)`.
5. Native: Freicoin header = (`nVersion, hashPrevBlock, merkle, nTime, nBits, nNonce`); PoW hash
   = its hash. Aux: Freicoin header = (`frc_*`, `merkle`); the header's aux-pow is `aux_pow`;
   PoW hash = hash of the Bitcoin header inside `aux_pow`, after `CheckAuxiliaryProofOfWork`
   succeeds with the share target substituted for the chain target.

## 5. The coinbase

The template a participant mines is a normal Freicoin block template except for its coinbase:

```
vin[0]:  height ‖ extranonce (as today)
vout[0..N-1]:  PPLNS payout outputs (§9), canonical order
vout[N]:       commitment output, value 0; as in the skeleton and in the block-final format,
               the commitment is the last 36 bytes of the serialized transaction before
               nLockTime: root (32) ‖ COMMIT_ID (4)
nLockTime
```

`cb1` is the serialization up to (not including) the root. The commitment root is a Merkle
hash map keyed by `SHARE_CHAIN_ID`, so a template may carry other commitments in the same
slot. For a share chain with one participant the payout list has one output.

Because the aux-pow commits to the whole Freicoin template (via `m_commit_hash_merkle_root`),
the Bitcoin header's hash covers the coinbase and therefore the payouts and the share header.

## 6. Validation

A share `S` with parent `P` (the share whose hash is the last leaf appended to `prev_shares`)
is valid iff:

1. **Format**: deserializes per §4; `version` recognized.
2. **Linkage**: `S.height == P.height + 1`; `S.prev_shares == P.prev_shares.Append(hash(P))`;
   `S.total_work == P.total_work + work(P.bits)`. Genesis: height 0, empty MMR, zero work.
3. **Target**: `S.bits` equals the target the chain prescribes at `S` (§7.3), and
   `S.bits ≤ MIN_SHARE_TARGET`.
4. **Proof of work**: PoW hash (§4.4) ≤ target(`S.bits`). Aux variant: the aux-pow is
   internally consistent (midstate, commit branch, aux branch) under the share target.
5. **Template plausibility**: `hashPrevBlock` (Freicoin) is a known block header no deeper than
   `MAX_TEMPLATE_AGE` (proposed: 2 blocks) below the Freicoin tip the verifier knows. This
   stops shares mined on stale templates from earning indefinitely.
6. **Payout**: the outputs decoded from `cb1` equal, byte for byte, `PPLNS(P, TOTAL)` (§9)
   where `TOTAL` is the sum of all `cb1` output values. `S.miner` must be one of the payees or
   the template builder.
7. **Timestamp**: Freicoin `nTime` within the usual block rules relative to the verifier's
   clock (±2 h) and not earlier than `P`'s `nTime` minus 10 minutes (anti-replay slack).
8. **Uniqueness**: the Bitcoin header hash (aux) or Freicoin header hash (native) has not been
   seen in another valid share.

A share that fails 5–8 is rejected without penalty to the peer; failing 1–4 is a protocol
violation and the peer is disconnected.

## 7. Chain selection, uncles and difficulty

### 7.1 Best chain

The best share chain is the valid chain with the greatest `total_work + work(tip.bits)`, ties
broken by first-seen. Reorganizations are expected and cheap: no state is attached to a share
beyond its index entry.

### 7.2 Uncles

> Proposed replacement: `doc/sharechain-uncles.md` (fruit-style crediting: no penalty, no
> includer reward, one recency parameter; fairness argument and simulation included).

A share `U` is an **uncle** of share `S` when: `U` is valid; `U` is not an ancestor of `S`;
`U`'s parent is an ancestor of `S` at height ≥ `S.height − UNCLE_DEPTH`; `U` has not been
included as an uncle by any ancestor of `S`. `S` lists its uncles by hash in a field added to
the share header in `version` ≥ 2 (`uncles`: CompactSize n + n × uint256; proposed, not in the
skeleton). An uncle contributes `(1 − UNCLE_PENALTY) × work(U.bits)` to its miner in PPLNS and
nothing to chain work.

### 7.3 Share target

The target for share `S` is computed from the chain it extends: linearly weighted moving
average (LWMA) of solve times over the last 60 shares with target spacing `SHARE_INTERVAL`,
clamped to ±25 % per share and to `MIN_SHARE_TARGET`. Uncles do not enter the average. A
chain with a single miner at 5 TH/s settles at a target ≈ 2^224 / 11.6 ≈ difficulty 11.6 k;
at 20 TH/s pool-wide ≈ 46 k.

## 8. Size and bandwidth

| Payees in window | Share size | Traffic at 10 s/share | 5-day window |
|---|---|---|---|
| 20 | ≈ 1.8 KB | 16 MB/day | 80 MB |
| 50 | ≈ 2.8 KB | 24 MB/day | 120 MB |
| 200 | ≈ 7.4 KB | 64 MB/day | 320 MB |

Breakdown: header ≈ 60 B + MMR peaks (≈ 32 × log2 n); `cb1` ≈ 80 B + 31 B per payout;
`branch` ≈ 32–160 B; `AuxProofOfWork` ≈ 440 B (observed: commit fields 48, secret 16, commit
branch 34, midstate 32 + ≤ 63 + 4, aux branch 6 × 32 for a 3 192-tx Bitcoin block, header 48).
`MAX_PAYEES = 100` caps a share at ≈ 4.4 KB.

## 9. PPLNS

Reference model with vectors: `contrib/sharechain/pplns_model.py`.

Inputs: the parent share `P` (the chain to walk), `TOTAL` (kria), the builder's address `B`.

1. **Walk** from `P` backwards through ancestors, accumulating `work(bits)` per miner, until
   the accumulated work reaches `W = WINDOW_K × E`, where `E` is the expected pool work per
   Freicoin block = `work(current Freicoin aux target)`. The share that crosses the boundary is
   counted in full. Each ancestor's listed uncles add `(1 − UNCLE_PENALTY) × work` to the
   uncle's miner.
2. **Split**: `out_m = floor(TOTAL × credit_m / Σcredit)`; the flooring remainder is added to
   `out_B` so that `Σout == TOTAL` exactly.
3. **Dust**: every `out_m < DUST` with `m ≠ B` is removed from `credit`; recompute step 2 once.
4. **Cap**: if more than `MAX_PAYEES` remain, keep the `MAX_PAYEES` largest credits and
   recompute step 2 once.
5. **Order**: outputs sorted by `scriptPubKey` bytes ascending. The verifier compares this
   vector with the decoded `cb1` outputs exactly.

The window slides with shares, not with blocks: a found Freicoin block does not reset it.
Variance for a participant with hashrate `h` in a pool with hashrate `H` shrinks by `H/h`
while expectation is unchanged.

### 9.1 Test vectors

`python3 -I contrib/sharechain/pplns_model.py` (seed 7, six miners, ≈ 8 % uncles,
`UNCLE_DEPTH 3`, `UNCLE_PENALTY 20 %`, `DUST 1000`):

| Shares | Window (typical shares) | Uncles | TOTAL (kria) | Outputs | sha256(canonical JSON)[:16] |
|---|---|---|---|---|---|
| 8 | 10 | 1 | 9 538 721 346 | 4 | `2269a3940bed3182` |
| 30 | 20 | 3 | 9 547 780 094 | 5 | `574bbe59252c0a53` |
| 120 | 50 | 6 | 9 541 454 263 | 6 | `df1a66351ede0550` |
| 400 | 200 | 22 | 9 547 824 785 | 6 | `50e276f5d2045949` |

An implementation must reproduce these digests on the same inputs.

## 10. Node integration (freicoind)

- **Template**: `GetWorkUnit`/`CustomizeWork` build the coinbase from `PPLNS(tip, TOTAL)` and
  append the share commitment for the client's address (the share header depends on `miner`,
  so per-address commits in `mining.aux.notify` are mandatory; they already exist).
- **Share target to bitcoind**: `mining.aux.notify` carries `bits`; freicoind sends the share
  target there. bitcoind then submits every hash that meets it (`mining.aux.submit` with the
  full `AuxProofOfWork`); freicoind distinguishes share from block. No mergemine change.
- **Submit path**: on `mining.aux.submit` (or native `mining.submit`): validate as §6, append
  to the index, relay; if it also meets the Freicoin target, continue into the existing
  second-stage/`ProcessNewBlock` path.
- **Index**: LevelDB `shares/` keyed by share hash: header, witness, parent, uncles, status.
  Prune below the window plus `UNCLE_DEPTH` and a safety margin (e.g. 2 × window).
- **P2P**: new messages `share` (one serialized share), `shareinv` (hashes), `getshares`
  (locator: up to 32 share hashes + count ≤ 500, answered with shares newest-first). Share
  traffic uses the existing Freicoin connections and a `NODE_SHARECHAIN` service bit.
- **Options**: `-sharechain=solo|main` (skeleton), `-defaultshareaddress` (skeleton),
  `-sharechainwindow=<K>`.
- **RPC**: `getsharechaininfo` extended with tip, height, work, window stats, estimated pool
  hashrate, per-miner shares in window.

## 11. Security considerations

- **Payout tampering**: a template paying anyone other than `PPLNS(P)` is not a valid share;
  if it becomes a Freicoin block, that is ordinary solo mining outside the chain and earns no
  share credit.
- **Block withholding**: a participant can withhold a found block, losing their own share of
  it; they gain nothing. Share withholding only reduces their own credit.
- **Stale-template farming**: rule §6.5 bounds how old a template may be.
- **Spam**: shares cost real hashes above `MIN_SHARE_TARGET`; per-peer rate limits apply to
  `share` messages whose chain is unknown to the receiver.
- **Reorg of the Freicoin chain**: shares reference a Freicoin `hashPrevBlock`; after a
  Freicoin reorg, shares on the orphaned branch remain valid shares (their work was real) and
  keep their credit. Only §6.5 can age them out.
- **Sybil payees**: splitting hashrate across addresses only adds outputs; `MAX_PAYEES` and
  `DUST` bound the cost, and small splits lose to the dust rule.
- **Time**: the share `nTime` is the template's time; manipulation within the allowed slack
  cannot change work or credit.

## 12. Open questions

1. `WINDOW_K` (3–5) and whether to express the window in Freicoin-block-work (as here) or in
   fixed share count as p2pool does.
2. Uncle inclusion reward (p2pool Monero pays the includer a slice of the penalty).
3. `MAX_TEMPLATE_AGE` and the exact timestamp slack.
4. Whether `prev_shares` peaks travel on the wire or only the MMR root (verifiers can rebuild).
5. Whether the commitment root should also commit to the uncle list, or uncles sit in the
   header (as proposed in §7.2).

## 13. Deliberate exclusions

Native (non-merge-mined) shares are specified but second priority. Mini/nano chains, light
share-chain clients, cross-chain (multi-aux) share chains and a share-chain explorer are out of
scope for a first implementation.
