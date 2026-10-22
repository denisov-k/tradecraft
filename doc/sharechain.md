
## 14. Implementation status (branch `sharechain`, 2026-10-22)

Implemented in freicoind on top of 28.1 and the `tmp` skeleton, following `doc/sharechain-uncles.md`
(fruits instead of uncles):

- share header `anchor ‖ prev_root ‖ fruits_root` (replaces the inline MMR), witness with
  `lock_height` and an aux variant carrying the full `AuxProofOfWork`; PoW of an aux share is the
  parent header hash (`GetAuxiliaryHash().first`);
- `GetShareWork`, `ComputeShareCredits`, `ComputeSharePayouts` with unit-test vectors
  (`contrib/sharechain/pplns_fruits.py`, `src/test/sharechain_tests.cpp`);
- `ShareChainstateManager`: fruit and chain-share acceptance (linkage, target, aux binding,
  payouts == PPLNS(anchor), fruit list rules), most-work tip, listable fruits, template state,
  LWMA retarget (main), `sharechain.dat` persistence, orphan pool;
- stratum: payout vector + `OP_RETURN <root‖4b4a4948>` last in the coinbase (before the segwit
  commitment is computed), share target to native miners and in `mining.aux.notify`, acceptance on
  `mining.submit` and `mining.aux.submit`, no credit for second-stage submissions;
- P2P: `NODE_SHARECHAIN` (1<<12), `share` and `getshares`, relay to peers with the bit;
- `getsharechaininfo` reports target, height, tip, chain shares, fruits.

Verified on regtest (`contrib/sharechain/regtest*`): single node two miners, merge mining through
mergemine bitcoind, two nodes with relay and restart. Not yet: mainnet parameters beyond the initial
target, share-chain sync for a node that joins late (only the orphan pool + `getshares`), bandwidth
limits, light-client proofs, the skeleton's `SOLO` type.
