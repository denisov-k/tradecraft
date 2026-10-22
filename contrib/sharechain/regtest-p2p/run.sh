#!/bin/bash
# Share chain P2P E2E: two nodes, each with its own miner; shares must propagate so that both nodes
# agree on the share chain and each node's coinbase pays both miners.
set -u
cd "$(dirname "$0")"
MINUTES="${1:-3}"
N1="docker exec frc-scp2p-node1 freicoin-cli -regtest -rpcuser=frc -rpcpassword=frcpass"
N2="docker exec frc-scp2p-node2 freicoin-cli -regtest -rpcuser=frc -rpcpassword=frcpass"
wait_rpc() { local n=0; until $1 getblockchaininfo >/dev/null 2>&1; do sleep 2; n=$((n+1)); if [ $n -gt 60 ]; then echo "timeout"; return 1; fi; done; return 0; }
sc() { $1 getsharechaininfo | python3 -c "import sys,json; d=json.load(sys.stdin); print('h', d.get('height'), 'cs', d.get('chainshares'), 'fr', d.get('fruits'), 'tip', (d.get('tip') or '')[:10])"; }

echo "== 1. bootstrap node1 (wallet + 110 blocks), node2 syncs"
docker compose --profile mine down -v >/dev/null 2>&1 || true
docker compose up -d node1 node2
wait_rpc "$N1" || exit 1; wait_rpc "$N2" || exit 1
$N1 createwallet w >/dev/null 2>&1; $N2 createwallet w >/dev/null 2>&1
ADDR1=$($N1 -rpcwallet=w getnewaddress "" bech32); ADDR2=$($N2 -rpcwallet=w getnewaddress "" bech32)
echo "ADDR1=$ADDR1  ADDR2=$ADDR2"
$N1 -rpcwallet=w generatetoaddress 110 "$ADDR1" >/dev/null && echo "generated 110 blocks on node1"
SHARE_ARG1="-defaultminingaddress=$ADDR1" SHARE_ARG2="-defaultminingaddress=$ADDR2" docker compose up -d --force-recreate node1 node2 >/dev/null 2>&1
wait_rpc "$N1" || exit 1; wait_rpc "$N2" || exit 1
sleep 5; echo "node2 height $($N2 getblockcount), peers $($N2 getconnectioncount); node1 peers $($N1 getconnectioncount)"
$N1 getpeerinfo | python3 -c "import sys,json; print('node1 peer services:', [p.get('services') for p in json.load(sys.stdin)])"

echo "== 2. mine on both nodes for $MINUTES min"
SHARE_ARG1="-defaultminingaddress=$ADDR1" SHARE_ARG2="-defaultminingaddress=$ADDR2" ADDR1="$ADDR1" ADDR2="$ADDR2" docker compose --profile mine up -d miner1 miner2 >/dev/null 2>&1
for i in $(seq 1 $((MINUTES*6))); do
  sleep 10
  printf "  t=%3ds  blocks %s/%s  node1: %s  node2: %s\n" $((i*10)) "$($N1 getblockcount)" "$($N2 getblockcount)" "$(sc "$N1")" "$(sc "$N2")"
done
docker compose --profile mine stop miner1 miner2 >/dev/null 2>&1
echo "== 3. result"
echo "node1: $(sc "$N1")"; echo "node2: $(sc "$N2")"
echo "-- share messages seen by node2:"; docker logs frc-scp2p-node2 2>&1 | grep -E 'share .* accepted from peer|share .* rejected|needs .* parents' | sed 's/^[^ ]* //' | cut -c1-45 | sort | uniq -c | sort -rn | head -5
echo "-- share messages seen by node1:"; docker logs frc-scp2p-node1 2>&1 | grep -E 'share .* accepted from peer|share .* rejected|needs .* parents' | sed 's/^[^ ]* //' | cut -c1-45 | sort | uniq -c | sort -rn | head -5
echo "-- last block coinbase on node1 (should pay both miners):"
tip=$($N1 getbestblockhash); $N1 getblock "$tip" 2 | python3 -c "
import sys,json; b=json.load(sys.stdin); cb=b['tx'][0]; print('height', b['height'], 'outputs', len(cb['vout']))
for o in cb['vout']: print('   ', o['value'], o['scriptPubKey'].get('type'), o['scriptPubKey'].get('address', o['scriptPubKey'].get('hex','')[:70]))"
echo "== 4. persistence: restart node1 and check the share chain is reloaded"
docker compose restart node1 >/dev/null 2>&1; wait_rpc "$N1" || exit 1
docker logs frc-scp2p-node1 2>&1 | grep -E 'Share chain: (dumped|loaded)' | tail -2 | cut -c1-160
echo "node1 after restart: $(sc "$N1")"
