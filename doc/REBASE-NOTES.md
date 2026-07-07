# Rebase notes: Freicoin/Tradecraft delta on Bitcoin Core 29.0

This branch carries the full Freicoin/Tradecraft consensus and feature
delta (tc-28.1, i.e. v28.1-42439) rebased onto Bitcoin Core `v29.0`,
squashed into a thematic review series. The tip tree is byte-identical
to the working `rebase-29` branch it was distilled from.

## Verification status

- All binaries build: `freicoind`, `freicoin-cli`, `freicoin-tx`,
  `freicoin-util`, `freicoin-wallet`, `freicoin-qt`, bench, fuzz.
- Unit tests: all 130 suites pass (`*** No errors detected`), including
  the demurrage golden vectors (`amount_tests`), `script_tests`,
  `sighash_tests`, `miner_tests`, `coins_tests`, the wallet suite.
- GUI tests: 21/21 pass (`QT_QPA_PLATFORM=offscreen`).
- Functional (python) suite: all previously failing suites fixed across
  six triage rounds.
- **Full mainnet revalidation**: the rebased node reindexed the entire
  Freicoin mainnet — 485,078 blocks, 1,370,740 transactions — with
  `-assumevalid=0`; every signature, demurrage calculation, block-final
  transaction and merged-mining proof validated. Final tip
  `3b2ec31858e6eeed52060c45fb358b99d7c0054ddaad3bb275bc52ced1df6b2e`
  is byte-identical to the tip reported by a v28.1-42439 node over the
  same chain.

## Open maintainer decisions

Two upstream-28.1→29.0 interactions need a call from the Freicoin
maintainers; both are wired to a conservative default and flagged in
the code:

1. **BIP94 timewarp vs. Freicoin time adjustment.** Core 29 introduced
   `GetMinimumTime()` (BIP94 timewarp mitigation, testnet4). Freicoin
   removed upstream's time-adjustment behavior long ago and keeps its
   own `original_adjust_interval`. This rebase wires `GetMinimumTime`
   to `original_adjust_interval`, preserving 28.1 consensus behavior on
   all existing chains. Whether Freicoin should *adopt* BIP94 semantics
   anywhere (e.g. a future testnet reset) is a maintainer decision.
   Markers: `consensus/params.h`, `rpc/mining.cpp`.

2. **PST vs. PSBT (BIP174).** Freicoin's PST format predates and
   diverges from BIP174. Core 29 continued to evolve PSBT; this rebase
   keeps Freicoin PST everywhere (`src/pst.*`, RPC names, wallet flow,
   `test_framework/pst.py`). Re-converging on BIP174 (bringing
   compatibility with external signers and coordinators) versus keeping
   PST is a maintainer decision; the rebase makes either path possible.

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
  (per tradecraftio/tradecraft#108), this series is the map for doing
  so.
- Vendored subtrees (secp256k1 et al.) carry tc-28.1's AGPL header
  convention over the v29 vendored code; upstream syncs of those
  subtrees will want the same treatment re-applied.
