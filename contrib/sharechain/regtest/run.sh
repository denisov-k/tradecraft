#!/bin/bash
# Share chain E2E: bootstrap a regtest chain, mine natively through freicoind's stratum with the
# share chain enabled, and check that fruits/chain shares accumulate and that found blocks carry
# the PPLNS payout vector plus the share commitment output.
set -u
cd "$(dirname "$0")"
MINUTES="${1:-2}"
F="docker exec frc-sc-freicoind freicoin-cli -regtest -rpcuser=frc -rpcpassword=frcpass"
wait_rpc() { local n=0; until $F getblockchaininfo >/dev/null 2>&1; do sleep 2; n=$((n+1)); if [ $n -gt 60 ]; then echo "timeout"; return 1; fi; done; return 0; }

echo "== 1. bootstrap: wallet + 110 blocks (ends IBD, matures a coinbase)"
docker compose --profile mine down -v >/dev/null 2>&1 || true
docker compose up -d freicoind
wait_rpc || exit 1
$F createwallet w >/dev/null 2>&1 || $F loadwallet w >/dev/null 2>&1 || true
FRC_ADDR=$($F -rpcwallet=w getnewaddress "" bech32)
FRC_ADDR2=$($F -rpcwallet=w getnewaddress "" bech32)
echo "FRC_ADDR=$FRC_ADDR  FRC_ADDR2=$FRC_ADDR2"
$F -rpcwallet=w generatetoaddress 110 "$FRC_ADDR" >/dev/null && echo "generated 110 blocks"
# restart with the real default share address (template builder for default credentials)
SHARE_ARG="-defaultminingaddress=$FRC_ADDR" FRC_ADDR="$FRC_ADDR" docker compose up -d --force-recreate freicoind >/dev/null 2>&1
wait_rpc || exit 1
echo "-- share chain params:"; $F getsharechaininfo

echo "== 2. mine natively through the stratum for $MINUTES min"
SHARE_ARG="-defaultminingaddress=$FRC_ADDR" FRC_ADDR="$FRC_ADDR" FRC_ADDR2="$FRC_ADDR2" docker compose --profile mine up -d pyminer pyminer2
h0=$($F getblockcount)
for i in $(seq 1 $((MINUTES*6))); do
  sleep 10
  info=$($F getsharechaininfo | python3 -c "import sys,json; d=json.load(sys.stdin); print('sc height', d.get('height'), 'chainshares', d.get('chainshares'), 'fruits', d.get('fruits'))")
  printf "  t=%3ds  blocks %s  %s\n" $((i*10)) "$($F getblockcount)" "$info"
done
docker compose --profile mine stop pyminer pyminer2 >/dev/null 2>&1
h1=$($F getblockcount)
echo "== 3. result: +$((h1-h0)) blocks"
echo "-- share chain log lines:"; docker logs frc-sc-freicoind 2>&1 | grep -E 'NEW SHARE CHAIN FRUIT|NEW CHAIN SHARE|Share chain:' | sed 's/^[^ ]* //' | cut -c1-60 | sort | uniq -c | sort -rn | head -8
echo "-- PPLNS window state:"; $F getsharechaininfo | grep -E 'height|chainshares|fruits'
echo "-- last block coinbase outputs (payouts + commitment):"
tip=$($F getbestblockhash); $F getblock "$tip" 2 | python3 -c "
import sys,json; b=json.load(sys.stdin); cb=b['tx'][0]
print('height', b['height'], 'coinbase outputs', len(cb['vout']))
for o in cb['vout']: print('   ', o['value'], o['scriptPubKey'].get('type'), o['scriptPubKey'].get('address', o['scriptPubKey'].get('hex','')[:80]))"
echo "-- pyminer tail:"; docker logs frc-sc-pyminer 2>&1 | tail -3 | cut -c1-120
