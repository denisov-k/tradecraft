# Rebase notes: Freicoin/Tradecraft delta on Bitcoin Core 31.1

This branch carries the full Freicoin/Tradecraft consensus and feature
delta (tc-28.1, i.e. v28.1-42439) rebased onto Bitcoin Core `v31.1`,
squashed into a thematic review series. The tip tree is byte-identical
to the working `rebase-31` branch it was distilled from.

It was reached in two hops from the published `rebase-29` work
(29 → 30 → 31.1); the intermediate `rebase-30` state is a build+unit
checkpoint only. The desktop Qt GUI is **frozen** at the 29 level and is
not built here (`-DBUILD_GUI=OFF`): a React Native mobile wallet is the
intended retail interface, so the GUI was not carried forward (see the
open question below).

## Verification status

- Binaries build: `freicoind`, `freicoin-cli`, `freicoin-tx`,
  `freicoin-util`, `freicoin-wallet` (bench and fuzz build; Qt not
  built — frozen).
- Unit tests: `test_bitcoin` reports `*** No errors detected` across the
  full suite (663 test cases), including the demurrage golden vectors
  (`amount_tests`), the data-driven consensus tests (`tx_valid`,
  `tx_invalid`, `script_json`), `sighash_tests`, `miner_tests`,
  `coins_tests`, reorg/assumeutxo and the wallet suite.
- Functional (python) suite: **`test_runner.py` ALL PASSED — 267/267**,
  0 failed (skips are the legacy-wallet variants only; Freicoin is
  sqlite-only, matching upstream).
- **Full mainnet revalidation**: a node on this code reindexed the
  entire Freicoin mainnet — 485,294 blocks — from the live network's
  block files with `-assumevalid=0`; every signature, demurrage
  calculation, block-final transaction and merged-mining proof was
  revalidated. Block hashes at heights 100,000 / 300,000 / 485,294 match
  a v28.1-42439 node byte-for-byte. Consensus equivalence of the rebase
  is proven end to end.

## Notes specific to the 31 hop

Bitcoin Core rewrote three subsystems between 29 and 31 that could not
be resolved hunk-by-hunk without producing non-compiling chimeras; each
was reconstructed from the pristine v31.1 file with the Freicoin delta
re-applied (`git merge-file`), then re-grafted:

- `script/miniscript.{h,cpp}` — v31's Node value-semantics rewrite.
- `node/miner.{h,cpp}` — v31 replaced ancestor-score selection with
  cluster-linearization; the block-final state machine, `vTxFees`
  placeholder and AuxPoW template budget were re-grafted onto it.
- `script/sign`, `signingprovider`, `scriptpubkeyman`, `descriptor` —
  the v31 MuSig2/taproot machinery was excised (Freicoin has no taproot;
  it uses MAST).

Two consensus-relevant regressions were caught by the unit tests and
fixed (both would matter on a live node):

1. `CCoinsViewCache::Reset()` — v31's new per-connect reset guard — must
   also clear `finalTxEntry`, or a reorg across a block-final boundary
   fatals `ConnectBlock` ("prior block-final tx hash not found").
2. `consensus/merkle.cpp` `MerkleComputation` lost the `*proot`/
   `*pmutated` output writes in the merge, giving wrong block-final and
   AuxPoW commitment hashes.

The recurring mechanical adaptation was v31's strong `script_verify_flags`
type (flag locals, `VerifyWitnessProgram`/`CheckInputScripts`/
`CScriptCheck` signatures, `TrimFlags`, `AllConsensusFlags`).

## Open maintainer decisions

Both consensus decisions flagged on the 29 rebase still stand, plus a
mobile/GUI direction question:

1. **BIP94 timewarp vs. Freicoin time adjustment.** Core kept
   `GetMinimumTime()` (BIP94 timewarp mitigation). Freicoin removed
   upstream's time-adjustment behavior long ago and keeps its own
   `original_adjust_interval`. This rebase wires `GetMinimumTime` to
   `original_adjust_interval`, preserving 28.1 consensus on all existing
   chains. Whether Freicoin should adopt BIP94 semantics anywhere is a
   maintainer decision. Markers: `consensus/params.h`, `rpc/mining.cpp`.

2. **PST vs. PSBT (BIP174).** Freicoin's PST format predates and
   diverges from BIP174. This rebase keeps Freicoin PST everywhere
   (`src/pst.*`, RPC names, wallet flow, `test_framework/pst.py`).
   Re-converging on BIP174 (external-signer/coordinator compatibility)
   versus keeping PST is a maintainer decision; the rebase makes either
   path possible.

3. **Qt GUI: freeze, port, or drop.** The Qt GUI was ported through the
   29 rebase but is frozen here (not built on 30/31). The delta still
   contains demurrage-aware GUI logic (~9 files: coincontrol,
   transactionrecord, walletmodel, …); porting it forward is possible if
   upstream wants a maintained desktop GUI. Our own direction is a light
   mobile wallet, so the GUI was frozen rather than carried.

## Known gaps

- **Release engineering unverified**: `depends/` cross-builds and the
  guix reproducible-build pipeline have not been exercised on this
  branch (resolved textually, never run). The NSIS Windows installer
  template is branded but untested.
- **Series granularity**: this is a thematic squash of the working
  rebase, structured for review; it is not the historical ~248-commit
  Tradecraft patch stack replayed 1:1. Intermediate commits of the
  series are not guaranteed to build in isolation; the tip is what is
  verified. If upstream prefers the full stack rebased commit-by-commit
  (per tradecraftio/tradecraft#108), this series is the map for doing so.
