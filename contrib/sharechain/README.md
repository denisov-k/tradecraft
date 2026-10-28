# Running a share chain node (operator notes)

The share chain lets independent merge-miners pool their Freicoin payouts without a pool
operator: every participant's freicoind builds templates whose coinbase pays all recent
contributors in proportion to work. Design: `doc/sharechain.md`; why fruits instead of
uncles: `doc/sharechain-uncles.md`. Status: tested on regtest only; the mainnet parameters
(spec §12) have not been exercised on real hashrate yet. This is what the first mainnet trial
needs.

## What you run

The same self-hosted merge-mining setup as before, with one change in freicoind's options:

```
bitcoind (mergemine)  --mergemine-->  freicoind -sharechain=main -stratum  <--P2P-->  other share chain nodes
   ^ your miners
```

- `freicoind` built from this branch (`sharechain`), on mainnet.
- `bitcoind` with the mergemine patch, pointed at freicoind's stratum as before
  (`-mergemine=<freicoind host>:9638`). mergemine needs no change: freicoind hands it the share
  target in `mining.aux.notify`, and every parent hash that meets it comes back as
  `mining.aux.submit`.
- Your ASICs point at bitcoind's stratum, unchanged.

Native (non merge-mined) mining straight against freicoind's stratum also works, with the same
share chain, if you want to test without bitcoind.

## freicoind options

```
server=1
stratum=1
stratumbind=0.0.0.0            # together with stratumallowip, otherwise localhost only
stratumallowip=<your LAN or the bitcoind host>
sharechain=main                # main | regtest; solo (default) = no chain
defaultshareaddress=fc1q...    # payout address for YOUR shares: a bech32 (witness) address;
                               # defaults to -defaultminingaddress
addnode=<another share chain node>   # at least one peer that runs the share chain
```

- `-defaultshareaddress` / `-defaultminingaddress` must be a witness address (`fc1q…`); the
  share chain pays witness programs only.
- A miner can also pass its own address as the stratum username (`fc1q…` or `fc1q…+diff`);
  shares are then credited to that address instead of the default.
- Share chain nodes find each other over the ordinary P2P network by the `NODE_SHARECHAIN`
  service bit, but on a fresh chain with two or three participants `-addnode` to a known one
  is the reliable way. Our node: `freicoin.ru` (ask for the port if it is not the default).

## What to expect

- `freicoin-cli getsharechaininfo` shows the chain: `height`, `tip`, `chainshares`,
  `fruits`, `sharebits`, `baseheight`.
- debug.log: `NEW SHARE CHAIN FRUIT (aux) by …` for every share, `NEW CHAIN SHARE (aux) …`
  for the one in six that extends the chain, `share … accepted from peer=` for relayed ones.
- The coinbase of a block you find pays every participant with shares in the window; your
  payout for a block someone else finds arrives in *their* coinbase, to your address.
- Share target on main starts at difficulty 10 000 (about one share per 10 s at 43 TH/s) and
  retargets per chain share (LWMA, ±25 % per step). With 30–40 TH/s pool-wide expect about one
  share every 10–15 s after it settles and one chain share per minute.
- `sharechain.dat` in the data dir keeps the chain across restarts.

## What we want to learn from a real run

1. Share and chain-share rate against the expected one (retarget behaviour, §7.3).
2. Whether both participants' addresses appear in each other's coinbases with the right
   proportions (`decoderawtransaction` of the coinbase of a found block).
3. Orphan/fork rate of chain shares between two nodes on the real network (`chainshares`
   minus `height` + 1, growing = forks).
4. Anything mergemine or the ASIC firmware does with the lower share target (rejects, stale
   work, reconnects).
5. Memory and bandwidth of freicoind over a day (`getsharechaininfo`, `getnettotals`).

Logs worth sending: `debug.log` lines matching `Share chain|SHARE|CHAIN SHARE|share .* peer`,
and `getsharechaininfo` once an hour.

## Building

As freicoind 28.1 (`doc/build-unix.md`): `./autogen.sh && ./configure --without-gui && make`.
Docker: `contrib/sharechain/Dockerfile.build` builds and tests the branch;
`contrib/sharechain/regtest*` are the regtest stands (single node, merge-mining through
mergemine, two nodes).
