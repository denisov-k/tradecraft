# Share chain end-to-end on regtest

`run.sh [minutes]` starts freicoind (image `freicoin/freicoind:sharechain`, built from this branch)
with `-sharechain=regtest`, bootstraps 110 blocks, then runs two Python CPU miners against the
native stratum. Every submission that meets the regtest share target is recorded as a fruit; one in
`chainfactor` as a chain share. The script prints the share chain height, counts of chain shares and
fruits, and the coinbase of the last block: the PPLNS payout outputs followed by the commitment
output `OP_RETURN <root || 4b4a4948>`.

Observed 2026-10-22 (3 minutes, two miners): 26 blocks, 27 chain shares at height 25 (two forks
resolved by work), last coinbase 37.5 + 12.5 FRC to the two miners + commitment.

## Sibling stacks

- `../regtest-aux/`: merge mining through mergemine bitcoind (image `freicoin/mergemine:v29.0.0.1`,
  see `prototypes/mergemine-stack` in the free-money workspace): shares arrive as
  `mining.aux.submit`, are accepted as aux fruits/chain shares, Freicoin blocks come through the
  two-stage path. Observed 2026-10-22: 15 aux fruits, 15 chain shares, FRC +10, BTC +15 in 4 min.
- `../regtest-p2p/`: two nodes, one miner each, `-connect` between them. Shares are relayed
  (`share`/`getshares`), both nodes end on the same share chain tip, each coinbase pays both
  miners, and node1 reloads its chain from `sharechain.dat` after a restart. Observed 2026-10-22:
  28 chain shares at height 25 on both nodes, blocks 136/136, payouts 37.5 + 12.5 FRC.
- `../Dockerfile.build`: the server build with a ccache cache mount (first build ~45 min, then ~8).
