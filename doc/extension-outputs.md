# Extension outputs: user-issued assets and the scrip ledger

Status: draft for review.  Branches of `denisov-k/tradecraft`, each on the previous, on `28.1`:

| branch | content | consensus |
|---|---|---|
| `scrip-interface` | `-scrip`: wallet RPC amounts relative to the scrip epoch | no |
| `extension-assets` | user-issued assets on extension outputs; wallet support | yes (`assets` deployment) |
| `scrip-ledger` | scrip as an asset backed by a pool in the block-final transaction | yes (same deployment) |

This follows the design described in chat (a witness-like per-output extension, value moved in and out
through anyone-can-spend pools in the block-final transaction, an inflationary freicoin as the simplest
asset) and Forward Blocks section XI.  Where it departs from the paper it says so (section 8); two questions are in section 9.

## 1. The extension push

A native witness output may already end in one data push of 2 to 75 bytes after its program and optional
shard specifier (`CScript::IsWitnessProgram`); consensus has ignored it so far, and protocol cleanup rule 6
exempts zero-valued outputs from `lock_height` monotonicity "for confidential transactions or non-freicoin
issued assets using extension outputs".  These branches give that push meaning without changing its
grammar: **its first byte is the extension version**.

```
scriptPubKey = <witness version> <program> [shard] <push: version byte || payload>
```

The owner of the output is its witness program, as before: old nodes require the same witness to spend it.
`CScript::GetExtensionOutput` returns the push, `CScript::WithoutExtension` the script without it.

| version | name | payload | output value |
|---|---|---|---|
| `0x01` | asset | asset id (32) ‖ amount (int64 LE) | 0 |
| `0x02` | pool | scrip (int64 LE) | the freicoin backing it |
| `0x03` | peg-in | 0 (int64) | the freicoin paid in |
| `0x04` | peg-out | scrip (int64 LE) | 0 |

Other versions mean nothing yet and are left to future rules.  A known version of the wrong length is
invalid (`bad-txns-asset-malformed`, `bad-txns-ledger-malformed`).

## 2. Deployment

BIP9 deployment `assets`, bit 4; `ALWAYS_ACTIVE` on regtest, not scheduled elsewhere; `gbt_force` (a miner
need not understand it to build a valid block).  Rules apply to blocks from activation and to the mempool
for the next block.  An extension in an output created below the activation height was never checked and
counts for nothing.

## 3. Assets (`0x01`)

- An asset output holds no freicoin (`bad-txns-asset-nonzero-value`); its amount is 1..`MAX_MONEY`.
- A transaction issues exactly one asset: `id = TaggedHash("Freicoin/asset-issuance", vin[0].prevout)`,
  in any amount and number of outputs.  The outpoint is spent once, so every asset is issued once and its
  supply is fixed.  A coinbase issues nothing.
- For every other asset the outputs carry no more than the inputs (`bad-txns-asset-in-belowout`); the rest
  is burned.  Several assets may move in one transaction, so a swap is one transaction.
- Assets carry no demurrage: the amount is the amount, whatever the reference height.
- Policy: an asset output and a peg-out are not dust; a peg-in may pay the (otherwise nonstandard) ledger program.

Code: `src/consensus/assets.{h,cpp}` (`GetAssetOutput`, `AssetIssuanceId`, `Consensus::CheckTxAssets`),
called from `ConnectBlock` and `MemPoolAccept::PreChecks`.

## 4. The scrip ledger (`0x02`–`0x04`)

**Units.** Scrip is freicoin valued at the scrip epoch, `FreicoinToScrip(value, height)` with
`SCRIP_EPOCH = 5040000` (already in `consensus/amount.cpp`).  Held scrip does not decay.  Before the epoch
scrip is the coarser unit (now about 1 scrip kria per 76 freicoin kria), so a payment of an exact scrip
amount is always representable (`FreicoinForScrip`); after the epoch it would not be, which is why the
epoch is in the future.  Scrip is an asset with the fixed id `TaggedHash("Freicoin/scrip")`.

**The ledger program.** The pool and peg-ins pay `LedgerBase() = OP_0 <'F' 'S'>`, an unassigned 2-byte
version 0 program that anyone can spend, so the block-final transaction can.

**Transaction rules** (`CheckTxAssets`):
- A peg-in output (value v > 0, on `LedgerBase`) lets its transaction mint up to
  `FreicoinToScrip(v, tx.lock_height)` scrip.  Nobody issues scrip otherwise.
- A peg-out output (value 0, on the recipient's program) consumes its scrip like an asset output.
- A peg-out is never spendable (`bad-txns-pegout-spend`); a peg-in or pool output only by the block-final
  transaction (`bad-txns-ledger-spend`); a pool output appears only in the block-final transaction.
- None of them in a coinbase.

**Block rules** (`BlockLedger`, `ComputeLedger`, `Consensus::CheckBlockLedger`):
- The prior pool is the pool output of the prior block-final transaction (if any): value V, scrip S.
- The block-final transaction must claim every peg-in of the block (`bad-blk-ledger-pegin-unclaimed`).
- Each peg-out of scrip s is paid `min(ScripToFreicoin(s, height), available)` in order, where
  `available` starts at `present(V) + Σ present(peg-in values)`.  The coinbase must carry an output to the
  peg-out's program of at least that much, one output per payout (`bad-blk-ledger-payout`).
- The new pool must be exactly `available` after the payouts, recording `S + Σ mintable − Σ s`, as one
  output before the last (commitment) output of the block-final transaction; no pool output when both are
  zero (`bad-blk-ledger-pool`, `bad-blk-ledger-pool-count`).
- Every rounding stays in the pool.  The recorded scrip bounds the scrip outstanding from above: a peg-in
  that mints less than it may leaves that freicoin in the pool.

**Why it is a soft fork.** To old nodes the pool is a trivially spendable block-final output, the peg-ins
are outputs the block-final transaction may spend (current-block outputs), and the payouts are fees that
the block-final transaction forwards to the coinbase.  The new rules only restrict.

**Reorgs.** A peg-out and its payout are in the same block and disappear together.

## 5. Node behaviour

- `BlockAssembler::settleLedger` claims the selected transactions' peg-ins, carries the pool and returns the
  payouts, which `CreateNewBlock` adds to the coinbase (taken from the miner's output).
- `getblocktemplate` adds `finaltx.pool` (the pool output to carry), lists peg-ins among
  `finaltx.prevout`, and adds `coinbasepayouts` (not included in `coinbasevalue`), for miners that build
  those transactions themselves.  The stratum server keeps the template's coinbase outputs and needs no
  change.
- `convertscrip amount ( "scrip"|"freicoin" height )` converts between the units.

## 6. Wallet

- Extension outputs are owned by their witness program: descriptor and cache lookups use
  `WithoutExtension`, so asset outputs are seen, solved and signed.
- Automatic coin selection and `sendall` never spend extension outputs (they would burn the asset);
  ledger outputs are never offered at all.  `bumpfee` and `fundrawtransaction` keep asset outputs as they
  are; asset change is never folded into freicoin change.
- RPCs: `issueasset`, `sendasset`, `getassetbalances`, `pegin`, `pegout`; `listunspent` shows `asset` and
  `asset_amount`.  Asset outputs go to native witness addresses only.  Fees are paid in freicoin.

`scrip-interface` (separate, no consensus): with `-scrip` the wallet RPCs report and accept amounts in
scrip, valuing each output at its reference height, so held amounts are constant; RPCs that cannot do this
refuse to run.

## 7. Tests

`assets_tests` (format, issuance, transfers, swaps, every rejection, pre-activation claims, ledger
transactions and settlement), `feature_assets.py`, `feature_scrip_ledger.py`, `wallet_assets.py`,
`wallet_scrip_ledger.py`, `wallet_scrip.py`; existing wallet, mempool, mining and block-final tests pass
unchanged.

## 8. Departures from Forward Blocks section XI

- **Suffix grammar.** The paper has up to eight pushes and a trailing version opcode; the grammar deployed
  with protocol cleanup has one push and no version.  This keeps the deployed grammar and puts the version
  in the first byte of the push.
- **Payouts** are paid by the coinbase of the same block, not through a payout queue with maturation (see
  question 1).
- **Fees** are paid in freicoin; fees in scrip (an explicit fee output, XI-B, or rebatable fees, XI-A) are
  not implemented.
- **Confidential amounts** would replace the amount in the extension with a commitment and put the range
  proof in an output witness committed in the block-final transaction; not started.

The epoch is `SCRIP_EPOCH = 5040000`, as already in `consensus/amount.cpp`.

## 9. Questions

1. **A payout queue?**  A peg-out and its payout share a block, so a reorg removes both and the payout
   then matures like any coinbase output.  Is that enough, or do you want peg-outs to go through a queue
   with a delay, as in the paper?
2. **Rates on assets?**  User-issued assets carry no demurrage here.  Should an asset be able to carry its
   own rate (as in Freimarkets), or are assets meant to sit outside the demurrage kernel?
