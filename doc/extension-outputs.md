# Extension outputs: user-issued assets, the scrip ledger, confidential scrip

Status: draft for review.  Branches of `denisov-k/tradecraft`, each on the previous, on `28.1`:

| branch | content | consensus |
|---|---|---|
| `scrip-interface` | `-scrip`: wallet RPC amounts relative to the scrip epoch | no |
| `extension-assets` | user-issued assets on extension outputs; wallet support | yes (`assets` deployment) |
| `scrip-ledger` | scrip as an asset backed by a pool in the block-final transaction | yes (same deployment) |
| `confidential-scrip` | output witnesses; scrip amounts hidden in Pedersen commitments | yes (`confidential` deployment) |

This follows the design described in chat (a witness-like per-output extension, value moved in and out
through anyone-can-spend pools in the block-final transaction, an inflationary freicoin as the simplest
asset) and Forward Blocks section XI.  Where it departs from the paper it says so (section 10); the open
questions are in section 11.

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
| `0x01` | asset | asset id (32) ‖ amount (int64 LE) [‖ `0x01`: demurrage] | 0 |
| `0x02` | pool | scrip (int64 LE) | the freicoin backing it |
| `0x03` | peg-in | 0 (int64) | the freicoin paid in |
| `0x04` | peg-out | scrip (int64 LE) | 0 |
| `0x05` | fee | scrip (int64 LE) | 0 |
| `0x06` | confidential scrip | Pedersen commitment (33) ‖ ephemeral public key (33) | 0 |

Other versions mean nothing yet and are left to future rules.  A known version of the wrong length is
invalid (`bad-txns-asset-malformed`, `bad-txns-ledger-malformed`).

## 2. Deployment

BIP9 deployment `assets`, bit 4; `ALWAYS_ACTIVE` on regtest, not scheduled elsewhere; `gbt_force` (a miner
need not understand it to build a valid block).  Rules apply to blocks from activation and to the mempool
for the next block.  An extension in an output created below the activation height was never checked and
counts for nothing.

## 3. Assets (`0x01`)

- An asset output holds no freicoin (`bad-txns-asset-nonzero-value`); its amount is 1..`MAX_MONEY`.
- A transaction issues an asset: `id = TaggedHash("Freicoin/asset-issuance", vin[0].prevout [‖ 0x01])`,
  in any amount and number of outputs.  The outpoint is spent once, so every asset is issued once and its
  supply is fixed.  A coinbase issues nothing.
- For every other asset the outputs carry no more than the inputs (`bad-txns-asset-in-belowout`); the rest
  is burned.  Several assets may move in one transaction, so a swap is one transaction.
- **Demurrage.**  An asset carries none by default: the amount is the amount, whatever the reference
  height.  An asset issued with demurrage (the flag byte `0x01` after the amount) decays exactly as
  freicoin does, by freicoin's own kernel (`GetTimeAdjustedValue`), from the output's reference height
  (the lock height of its transaction).  That is what a claim on freicoin needs (an exchange deposit, an
  IOU, a pool share): one that kept its amount while freicoin decayed would pay to hold instead of the
  coins.  The id commits to the flag, every output and input of an asset agrees on it
  (`bad-txns-asset-demurrage`), and scrip has none.  Inputs are valued at the transaction's lock height,
  which may not be below such an input's reference height (`bad-txns-asset-lock-height`; zero-value
  outputs are otherwise exempt from monotonicity).  Other rates (Freimarkets) are not supported; a
  later extension version could add them as a soft fork.
- Policy: an asset output and a peg-out are not dust; a peg-in may pay the (otherwise nonstandard) ledger program.

Code: `src/consensus/assets.{h,cpp}` (`GetAssetOutput`, `AssetIssuanceId`, `Consensus::CheckTxAssets`),
called from `ConnectBlock` and `MemPoolAccept::PreChecks`.

## 4. The scrip ledger (`0x02`–`0x05`)

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
- A fee output (value 0, on `LedgerBase`) consumes its scrip as a fee to the miner.
- A peg-out or a fee is never spendable (`bad-txns-pegout-spend`); a peg-in or pool output only by the
  block-final transaction (`bad-txns-ledger-spend`); a pool output appears only in the block-final
  transaction.
- None of them in a coinbase.

**Block rules** (`BlockLedger`, `ComputeLedger`, `Consensus::CheckBlockLedger`):
- The prior pool is the pool output of the prior block-final transaction (if any): value V, scrip S.
- The block-final transaction must claim every peg-in of the block (`bad-blk-ledger-pegin-unclaimed`).
- **The payout queue.** A peg-out made in block h is paid by block h + D (`ledger_payout_delay`: 1 on
  mainnet, 10 on regtest).  What a block owes is thus fixed by the chain below it, read from block h − D
  (`GetDuePegouts`, `BlockPegouts`); peg-outs made before activation are not paid.
- Each due peg-out of scrip s is paid `min(ScripToFreicoin(s, height), available)` in order, where
  `available` starts at `present(V)`: the prior pool, which backs the queued scrip, so the payouts follow
  from the chain below.  The coinbase must carry an output to the peg-out's program of at least that much,
  one output per payout (`bad-blk-ledger-payout`).
- Then the fees in scrip leave the pool, the block's peg-ins in: their value is
  `min(ScripToFreicoin(Σ fee scrip, height), available + Σ present(peg-in values))`, which the block-final
  transaction forwards to the coinbase as a fee (so the coinbase rule is unchanged).
- The new pool must be exactly `available` after the payouts, plus `Σ present(peg-in values)`, less the
  fees' value, recording `S + Σ mintable − Σ s − Σ fee scrip` (a queued peg-out stays in S until paid),
  as one output before the last (commitment) output of the block-final transaction; no pool output when
  both are zero (`bad-blk-ledger-pool`, `bad-blk-ledger-pool-count`).
- Every rounding stays in the pool.  The recorded scrip bounds the scrip outstanding from above: a peg-in
  that mints less than it may leaves that freicoin in the pool.

**Why it is a soft fork.** To old nodes the pool is a trivially spendable block-final output, the peg-ins
are outputs the block-final transaction may spend (current-block outputs), and the payouts are fees that
the block-final transaction forwards to the coinbase.  The new rules only restrict.

**Reorgs.** A payout follows its peg-out by D blocks: a reorg shallower than D cannot touch a payout, and
a deeper one replays both.

**The share chain.** A block found through the share chain is also a share, whose coinbase peers rebuild
as the share payouts plus the commitment.  Because what a block owes follows from the chain below it, a
share-chain node can compute the due payouts for the block a share builds on and rebuild the coinbase
with them, without the share carrying them (branch `sharechain-ledger`).

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
- RPCs: `issueasset` (with an optional `demurrage`), `sendasset`, `getassetbalances`, `pegin`, `pegout`
  (`sendasset` of scrip and `pegout` take `fee_in_scrip`: scrip inputs only, the fee burned in scrip);
  `listunspent` shows `asset` and `asset_amount`, and for an asset with demurrage `asset_demurrage` and
  `asset_present`.  Balances and sends value such an asset at the next block.  Asset outputs go to native
  witness addresses only.
- A fee paid in scrip counts at its freicoin value in fee estimation, and `bumpfee` raises it in scrip, out
  of the scrip change, with the same inputs.

`scrip-interface` (separate, no consensus): with `-scrip` the wallet RPCs report and accept amounts in
scrip, valuing each output at its reference height, so held amounts are constant; RPCs that cannot do this
refuse to run.

## 7. Output witnesses

The data structure Forward Blocks (XI-B, footnote 20) gives outputs for range proofs, here general.

- **Transaction.**  Beside `vout`, a vector of witness stacks, one per output (`output_witness`, empty or
  full size).  It is serialized after the input witnesses under **bit 2 of the extended format's flags**,
  the byte after the `0xff` marker that was reserved for such extensions; an all-empty record is invalid,
  as for input witnesses.  `TX_WITH_WITNESS` carries both, `TX_WITH_INPUT_WITNESS` only the input
  witnesses.
- **Hashes and weight.**  Neither the txid nor the **wtxid** covers output witnesses, so a node without
  these rules computes the same witness commitment.  They count as witness data in the weight.
- **Block commitment.**  The witness commitment in the block-final transaction is the root of a small
  tree: the witness root, then a Merkle branch of up to seven hashes carried in the coinbase witness,
  which consensus has left unconstrained.  With these rules its **first hash is the root of a fast Merkle
  tree of the transactions' output witness hashes** (`BlockOutputWitnessMerkleRoot`; a leaf is
  `CTransaction::GetOutputWitnessHash`, zero for a transaction without output witnesses).  It is required
  in every block, so a block stripped of its output witnesses fails it as **mutated**, not invalid
  (`bad-output-witness-merkle-match`); without the branch the block is invalid
  (`bad-output-witness-commitment`).  The miner and the stratum server build the commitment one level up
  (path `0x02`); `getblocktemplate` gives `outputwitnessroot`.
- **Network.**  Service bit 13, `NODE_OUTPUT_WITNESS` (12 is the share chain's).  Peers with it get
  transactions and blocks whole; others get them without output witnesses, which their commitment check
  accepts, and are never announced a transaction that has any.  A `getblocktxn` from such a peer for one
  is answered with the whole block, stripped.  Under the rules blocks are downloaded only from peers with
  the bit, a stripped block from a peer without it is dropped without blaming the peer, and the bit
  joins the desirable service flags, so outbound connections go to such peers (as `NODE_WITNESS` did for
  segwit).
- **Deployment.**  BIP9 `confidential`, bit 5, not active on any network yet, regtest included (tests opt
  in with `-vbparams=confidential:-1:9223372036854775807`); not `gbt_force`, as the coinbase witness must
  carry the root.  Before it the mempool refuses output witnesses (`output-witness-premature`) and a block
  with any is mutated.

## 8. Confidential scrip (`0x06`)

Only scrip: freicoin and assets with demurrage decay by a fractional factor with rounding, which a
commitment cannot follow without revealing the amount.  The pool, peg-ins, peg-outs and fees stay explicit,
so the ledger rules of section 4 do not change.

- **Output.**  Extension `0x06`: a Pedersen commitment to the amount (33 bytes) and the sender's ephemeral
  public key (33 bytes), 67 bytes, within the push; no freicoin (`bad-txns-ct-nonzero-value`).  Never in
  the coinbase (`bad-txns-ct-coinbase`) or the block-final transaction.  The generator is derived from the
  scrip asset id (`secp256k1_generator_generate`), so assets without demurrage could be confidential the
  same way later.
- **Range proof.**  The single item of the output's witness (`bad-txns-ct-proof-missing`), at most 5134
  bytes, a Borromean range proof (secp256k1-zkp `rangeproof`, as in Elements/Liquid) bound to the output's
  program (`extra_commit`), and proving at most `MAX_MONEY` (`bad-txns-ct-range`).
- **Balance.**  A transaction with confidential inputs or outputs balances its scrip by the commitments:
  those of its confidential inputs plus the explicit scrip in (scrip inputs, scrip minted by peg-ins) equal
  those of its confidential outputs plus the explicit scrip out (scrip outputs, peg-outs, fees), the
  explicit amounts committed with a zero blinding factor (`secp256k1_pedersen_verify_tally`,
  `bad-txns-ct-balance`).  That replaces the explicit check of scrip for the transaction.
- **Verification budget.**  A block may give at most 65536 range-proof bits to check
  (`MAX_BLOCK_CONFIDENTIAL_BITS`, `bad-blk-ct-cost`), counted from the proofs' headers
  (`secp256k1_rangeproof_info`) before any is verified; a missing or unreadable proof counts 64.
  Verification takes about 0.11 ms a bit on one core of the test server (`bench_rangeproof`; an ECDSA
  check takes 0.06 ms), so that is some 7 s, about 1260 outputs of 52 bits; with size expansion a block's
  weight (up to 2 GiB) would not bound it.  The miner keeps within it; policy gives a transaction a tenth
  (`ct-cost`).
- **Activation.**  Before the rules a `0x06` output is an unknown extension; one created then counts for
  nothing after.  Relay policy: confidential outputs are not dust; output witnesses are standard only on
  them.
- **Cryptography.**  The `generator` and `rangeproof` modules of secp256k1-zkp, taken verbatim from
  `BlockstreamResearch/secp256k1-zkp` 211323d6 (the state closest to our secp256k1 v0.5.1) with the four
  helpers they need from its core, enabled by the node's configure; their own tests pass on our copy.
  The `ecdh` module is enabled for the wallet.

**Wallet.**
- Blinding keys derive from a master blinding key (`ctmasterkey` in the wallet database, unencrypted: it
  reveals amounts but spends nothing) and the output program.
- `getconfidentialaddress` gives an address, its blinding public key, and both in one **confidential
  address**: blech32m (Elements' variant of bech32m with a 12-character checksum, made for addresses this
  long; `src/blech32.{h,cpp}` from Elements, passing its test vectors) under the network's prefix with `c`
  before it (`cfc1...`, `ctf1...`, `cfcrt1...`), the witness version then the blinding key and the
  program.  `sendconfidential address amount
  ( blindingkey )`, given a confidential address or an address and a blinding key, spends explicit and confidential scrip; with the recipient's blinding key the payment is
  confidential, without it explicit scrip; the change is confidential (so the payment's blinding factor is
  random), and the fee is paid in scrip, so no freicoin is needed.  An ephemeral key's ECDH with the
  recipient's blinding key seeds the range proof (52 private bits), which the recipient rewinds to recover
  the amount and the blinding factor.  The proofs are added after signing, which does not cover output
  witnesses.  `listconfidential` lists what the wallet holds, unblinded.
- `getrawtransaction` and the like show a `witness` array on outputs that have one.

## 9. Tests

`assets_tests` (format, issuance, transfers, swaps, every rejection, pre-activation claims, ledger
transactions and settlement), `feature_assets.py`, `feature_scrip_ledger.py`, `wallet_assets.py`,
`wallet_scrip_ledger.py`, `wallet_scrip.py`; `output_witness_tests` (serialization, hashes, weight, the
block commitment), `confidential_tests` (real commitments and proofs: balance, missing and misbound proofs,
a proof beyond `MAX_MONEY`, pre-activation commitments), `wallet_confidential.py` (end to end, two nodes);
the `scrip_ledger` fuzz target; existing wallet, mempool, mining, segwit, compact block and block-final
tests pass unchanged.

## 10. Departures from Forward Blocks section XI

- **Suffix grammar.** The paper has up to eight pushes and a trailing version opcode; the grammar deployed
  with protocol cleanup has one push and no version.  This keeps the deployed grammar and puts the version
  in the first byte of the push.
- **Payouts** go through a queue as in the paper, with a fixed delay rather than lock-in and maturation:
  the coinbase D blocks later pays them, D = 1 on mainnet (the next block).  The queue is what the share
  chain needs: a share's coinbase is rebuilt by its peers, who cannot know a block's own peg-outs but can
  compute the payouts due from the chain below.  One block is enough for that, and a longer delay buys no
  safety: the payouts follow from the chain, so a reorg that drops a peg-out drops its payout with it, and
  a delay only means the peg-out has D confirmations when paid, which a recipient waits for anyway.  Each
  block of delay is ten minutes a user waits.  We read the paper's lock-in and maturation as needed for
  the peg between two chains in Forward Blocks, which a single chain does not have.  Regtest keeps D = 10
  so the queue is visible in tests.
- **Fees in scrip** follow the paper's explicit fee (XI-B, mechanism 1): a fee output (`0x05`, on the
  pool's program, no freicoin) burns scrip, and the block's pool gives up its freicoin value at the block
  height, after the payouts and with the block's peg-ins in.  To old nodes that is a fee of the
  block-final transaction, which the coinbase may already claim, so the coinbase rule is unchanged.  In
  the mempool the value counts as a modified fee (relay, ranking, RBF), not a base fee, as the block-final
  transaction forwards it.  Rebatable fees (XI-A) are not implemented.
- **Confidential amounts** follow XI-B, adapted to what Freicoin already deployed:
  - The commitment and the nonce key sit in the extension push itself (67 bytes) rather than a hash of
    them.  That is what the push was made for: "an unconstrained field ... for things like confidential
    transactions or issued assets, or commitments to these values" (release notes 13.2), and confidential
    outputs are the zero-valued outputs protocol cleanup rule 6 exempts from `lock_height` monotonicity
    with them in mind.  Neither fixes a layout beyond that.
  - The output witness root goes where the paper suggests, the segwit witness nonce, which in Freicoin is
    the coinbase witness holding the witness commitment's branch: its first hash.
  - Only scrip is confidential (section 8): a commitment cannot follow demurrage's fractional factor without
    revealing the amount, so this is not a choice.
  - The range proofs are Borromean (secp256k1-zkp `rangeproof`, in production in Liquid since 2018), not
    Bulletproofs, which would be about five times smaller but have no mature implementation in
    secp256k1-zkp proper; a later extension version can switch.
  - Explicit-to-confidential and back is a plain transaction, as scrip is one asset whichever form it
    takes; the payout queue is used only by peg-outs.

The epoch is `SCRIP_EPOCH = 5040000`, as already in `consensus/amount.cpp`.

## 11. Questions

1. **Rates on assets.**  An asset may decay as freicoin does, or not at all (§3), which is what a claim on
   freicoin needs.  Do you want the Freimarkets rates (any rate, or interest) on chain as well?
