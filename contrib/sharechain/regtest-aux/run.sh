#!/bin/bash
# End-to-end regtest merge mining: freicoind (aux chain) <- mergemine bitcoind (parent) <- python miner.
# Usage: ./run.sh [minutes-to-mine]   (default 3)
set -u
cd "$(dirname "$0")"
MINUTES="${1:-3}"
FRC="docker exec frc-scaux-freicoind freicoin-cli -regtest -rpcuser=frc -rpcpassword=frcpass"
BTC="docker exec frc-scaux-bitcoind bitcoin-cli -regtest -rpcuser=btc -rpcpassword=btcpass"

wait_rpc() { local n=0; until $1 getblockchaininfo >/dev/null 2>&1; do sleep 2; n=$((n+1)); [ $n -gt 60 ] && { echo "timeout waiting for $2"; return 1; }; done; echo "$2 RPC up"; }

echo "== 1. start freicoind, create wallet, bootstrap the chain (450 native blocks: ends IBD and activates auxpow)"
docker compose --profile mine down -v >/dev/null 2>&1 || true
docker compose up -d freicoind
wait_rpc "$FRC" freicoind || exit 1
$FRC createwallet w >/dev/null 2>&1 || $FRC loadwallet w >/dev/null 2>&1 || true
FRC_ADDR=$($FRC -rpcwallet=w getnewaddress)
echo "FRC_ADDR=$FRC_ADDR"
$FRC -rpcwallet=w generatetoaddress 450 "$FRC_ADDR" >/dev/null && echo "generated 450 blocks"
$FRC getblockchaininfo | grep -E '"(blocks|initialblockdownload)"' | tr -d ' ' | tr '\n' ' '; echo
echo -n "auxpow deployment: "; $FRC getdeploymentinfo | python3 -c "import sys,json; d=json.load(sys.stdin)['deployments'].get('auxpow',{}); print(d.get('active'), d.get('bip9',{}).get('status'))" 2>/dev/null || echo "?"

# regtest generatetoaddress pushes block times ahead of the wall clock (nTime = max(MTP+1, now)); an aux block
# inherits the Bitcoin parent's (real-time) nTime, so wait until real time passes Freicoin's median-time-past.
mtp=$($FRC getblockchaininfo | python3 -c "import sys,json; print(json.load(sys.stdin)['mediantime'])"); now=$(date +%s)
if [ "$mtp" -ge "$now" ]; then echo "waiting $((mtp-now+5))s for wall clock to pass regtest MTP"; sleep $((mtp-now+5)); fi

echo "-- restart freicoind with -defaultminingaddress (share chain template builder)"
SHARE_ARG="-defaultminingaddress=$FRC_ADDR" FRC_ADDR="$FRC_ADDR" docker compose up -d --force-recreate freicoind >/dev/null 2>&1
wait_rpc "$FRC" freicoind || exit 1
$FRC getsharechaininfo | grep -E 'network|sharebits'
echo "== 2. start mergemine bitcoind pointed at freicoind stratum with that payout address"
SHARE_ARG="-defaultminingaddress=$FRC_ADDR" FRC_ADDR="$FRC_ADDR" docker compose up -d bitcoind
wait_rpc "$BTC" bitcoind || exit 1
$BTC createwallet w >/dev/null 2>&1 || $BTC loadwallet w >/dev/null 2>&1 || true
BTC_ADDR=$($BTC -rpcwallet=w getnewaddress)
echo "BTC_ADDR=$BTC_ADDR"
$BTC -rpcwallet=w generatetoaddress 101 "$BTC_ADDR" >/dev/null && echo "bitcoind: 101 blocks generated (ends IBD; one mature coinbase for the block-final tx)"
sleep 4
echo "-- mergemine client: aux work status"
docker logs frc-scaux-bitcoind 2>&1 | grep -iE "aux.*(notify|work|error)|initial sync" | tail -4 | cut -c1-200

echo "== 3. start python miner against bitcoind stratum for $MINUTES min"
SHARE_ARG="-defaultminingaddress=$FRC_ADDR" FRC_ADDR="$FRC_ADDR" BTC_ADDR="$BTC_ADDR" docker compose --profile mine up -d pyminer
h0_frc=$($FRC getblockcount); h0_btc=$($BTC getblockcount)
for i in $(seq 1 $((MINUTES*6))); do
  sleep 10
  printf "  t=%3ds  FRC height %s  BTC height %s\n" $((i*10)) "$($FRC getblockcount)" "$($BTC getblockcount)"
done
docker compose --profile mine stop pyminer >/dev/null 2>&1
h1_frc=$($FRC getblockcount); h1_btc=$($BTC getblockcount)
echo "== 4. result: FRC +$((h1_frc-h0_frc)) blocks, BTC +$((h1_btc-h0_btc)) blocks"
echo "-- pyminer tail:"; docker logs frc-scaux-pyminer 2>&1 | tail -6 | cut -c1-160
echo -n "-- freicoind GOT AUXILIARY BLOCK lines: "; docker logs frc-scaux-freicoind 2>&1 | grep -c "GOT AUXILIARY BLOCK"
echo "-- share chain:"; $FRC getsharechaininfo | grep -E 'height|chainshares|fruits'; docker logs frc-scaux-freicoind 2>&1 | grep -E 'SHARE CHAIN FRUIT|NEW CHAIN SHARE|Share chain:' | sed 's/^[^ ]* //' | cut -c1-50 | sort | uniq -c | sort -rn | head -6
echo "-- FRC wallet balance:"; $FRC -rpcwallet=w getbalances | grep -E '"(trusted|immature)"'
tip=$($FRC getbestblockhash); $FRC getblockheader "$tip" false > /tmp/frc-tip-header.hex
echo "FRC_TIP=$tip (header $(wc -c < /tmp/frc-tip-header.hex) hex chars)"
