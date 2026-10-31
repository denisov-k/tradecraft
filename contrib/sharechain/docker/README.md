# Home mining on the share chain with Docker

One command starts a Freicoin node with the share chain and a stratum server for the miners
on your home network. Each share your hardware finds is a fruit on the share chain; when
anyone on the share chain finds a Freicoin block, the reward is split by work over the window
(doc/sharechain.md §9) and your part goes straight to your address in the coinbase.

## Start

1. Install Docker (Docker Desktop on Windows or macOS, `docker.io` + the compose plugin on
   Linux). About 3 GB of disk for the Freicoin node, ~15 GB more with merge mining.
2. Copy `.env.example` to `.env` and set `FRC_ADDR` to your `fc1q…` address.
3. In this directory:

       docker compose up -d --build

   The first build compiles freicoind (20–60 minutes); the node then syncs the Freicoin chain
   (one to three hours) and finds the share chain on its own (it connects to the chain's known
   nodes).
4. Point each miner at this computer:

   | setting | value |
   |---|---|
   | pool URL | `stratum+tcp://<this computer's LAN IP>:9638` |
   | username | your `fc1q…` address, optionally `.rigname` (anything else mines to `FRC_ADDR`) |
   | password | anything |

Only machines on home networks (192.168.x.x, 10.x.x.x) and Docker itself can use the stratum
port; it is not open to the internet.

## Merge mining with Bitcoin

To earn Freicoin with the same work you do for Bitcoin, set `BTC_ADDR` in `.env` and start
the `mergemine` profile:

    docker compose --profile mergemine up -d --build

Then point miners at port `3333` instead: they mine Bitcoin through a pruned bitcoind (Mark
Friedenbach's mergemine build, with Vintage Hash's fixes for home hardware), and every share
that meets the Freicoin share chain's target is a fruit as well. The Bitcoin node needs a few
hours to sync before work flows.

## Watching it

    docker compose exec freicoind freicoin-cli -datadir=/data -rpcuser=frc -rpcpassword=<FRC_RPC_PASSWORD> getsharechaininfo
    docker compose exec freicoind freicoin-cli -datadir=/data -rpcuser=frc -rpcpassword=<FRC_RPC_PASSWORD> getsharecredits
    docker compose logs -f freicoind | grep -i "share\|stratum"

`getsharecredits` shows your fruits and work in the window, and what a block found now would
pay you. Credits can be sold for sats or FRC before a block is found:
`contrib/sharechain/swap/README.md`.

## Notes

- Stop with `docker compose stop` (it waits for freicoind to save its state), not `kill`.
- `TC_REF` in `.env` picks the branch freicoind is built from. The node is built without a
  wallet: it holds no keys, payouts go to `FRC_ADDR`.
- Small miners while a very large one is on the share chain: see
  doc/sharechain-light-fruits.md.
