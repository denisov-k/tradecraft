#!/bin/bash
# Share-credit swap E2E on the two-node stand, trust mode (no lightning): miner1 mines to a
# key address on node1, then sells its credits to a market maker that is paid on node2's
# address.  Expected: the seller's credits move to the maker's address on both nodes and the
# next block's coinbase pays the maker, not the seller.
# usage: swap.sh [minutes of mining, default 2]   (run from contrib/sharechain/regtest-p2p)
set -u
cd "$(dirname "$0")"
MINUTES="${1:-2}"
N1="docker exec frc-scp2p-node1 freicoin-cli -regtest -rpcuser=frc -rpcpassword=frcpass"
N2="docker exec frc-scp2p-node2 freicoin-cli -regtest -rpcuser=frc -rpcpassword=frcpass"
PY="docker run --rm --network frc-scp2p_p2pnet -v $(pwd)/../swap:/swap:ro python:3.12-slim python -u"
wait_rpc() { local n=0; until $1 getblockchaininfo >/dev/null 2>&1; do sleep 2; n=$((n+1)); if [ $n -gt 60 ]; then echo "timeout"; return 1; fi; done; return 0; }
owners() { $1 getsharecredits | python3 -c "import sys,json; print({c['address'][:14]: c['fruits'] for c in json.load(sys.stdin)['credits']})"; }

read -r WIF SELLER < <(python3 ../swap/newkey.py regtest)
echo "== 0. seller key address $SELLER"

echo "== 1. bootstrap: node1 mines to the seller's key address, node2 to its wallet address"
docker compose --profile mine down -v >/dev/null 2>&1 || true
docker compose up -d node1 node2 >/dev/null 2>&1
wait_rpc "$N1" || exit 1; wait_rpc "$N2" || exit 1
$N2 createwallet w >/dev/null 2>&1
MAKER=$($N2 -rpcwallet=w getnewaddress "" bech32)
$N1 generatetoaddress 110 "$SELLER" >/dev/null && echo "generated 110 blocks; maker address $MAKER"
SHARE_ARG1="-defaultminingaddress=$SELLER" SHARE_ARG2="-defaultminingaddress=$MAKER" docker compose up -d --force-recreate node1 node2 >/dev/null 2>&1
wait_rpc "$N1" || exit 1; wait_rpc "$N2" || exit 1

echo "== 2. mine on both nodes for $MINUTES min"
SHARE_ARG1="-defaultminingaddress=$SELLER" SHARE_ARG2="-defaultminingaddress=$MAKER" ADDR1="$SELLER" ADDR2="$MAKER" docker compose --profile mine up -d miner1 miner2 >/dev/null 2>&1
sleep $((MINUTES * 60))
docker compose --profile mine stop miner1 >/dev/null 2>&1   # the seller stops; miner2 keeps the chain going
echo "node1 credits: $(owners "$N1")"

echo "== 3. market maker on node2 (trust mode), seller on node1"
docker rm -f frc-scp2p-maker >/dev/null 2>&1 || true
docker run -d --name frc-scp2p-maker --network frc-scp2p_p2pnet -v "$(pwd)/../swap:/swap:ro" python:3.12-slim python -u /swap/maker.py \
  --rpc http://node2:28638 --rpcuser frc --rpcpassword frcpass --address "$MAKER" --sats-per-unit 1000 --min-sats 0 \
  --expiry 20 --min-left 3 --no-lightning --listen 0.0.0.0:9640 >/dev/null
sleep 3
$PY /swap/sell.py --rpc http://node1:28638 --rpcuser frc --rpcpassword frcpass --wif "$WIF" --address "$SELLER" \
  --maker http://frc-scp2p-maker:9640 --no-lightning --timeout 300
RC=$?
echo "-- maker log:"; docker logs frc-scp2p-maker 2>&1 | tail -5
docker rm -f frc-scp2p-maker >/dev/null 2>&1

echo "== 4. result (sell rc=$RC)"
sleep 15
echo "node1 credits: $(owners "$N1")"; echo "node2 credits: $(owners "$N2")"
echo "pending on node1: $($N1 listsharetxs | python3 -c "import sys,json; print(len(json.load(sys.stdin)['pending']))")"
sleep 20
tip=$($N2 getbestblockhash); $N2 getblock "$tip" 2 | python3 -c "
import sys,json; b=json.load(sys.stdin); cb=b['tx'][0]; print('last block', b['height'], 'coinbase outputs', len(cb['vout']))
for o in cb['vout']: print('   ', o['value'], o['scriptPubKey'].get('address', o['scriptPubKey'].get('hex','')[:40]))"
echo "seller $SELLER should be gone, maker $MAKER should hold its fruits"
docker compose --profile mine stop miner2 >/dev/null 2>&1
echo "== swap done"
