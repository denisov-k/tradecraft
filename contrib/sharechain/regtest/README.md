# Share chain end-to-end on regtest

`run.sh [minutes]` starts freicoind (image `freicoin/freicoind:sharechain`, built from this branch)
with `-sharechain=regtest`, bootstraps 110 blocks, then runs two Python CPU miners against the
native stratum. Every submission that meets the regtest share target is recorded as a fruit; one in
`chainfactor` as a chain share. The script prints the share chain height, counts of chain shares and
fruits, and the coinbase of the last block: the PPLNS payout outputs followed by the commitment
output `OP_RETURN <root || 4b4a4948>`.

Observed 2026-10-22 (3 minutes, two miners): 26 blocks, 27 chain shares at height 25 (two forks
resolved by work), last coinbase 37.5 + 12.5 FRC to the two miners + commitment.
