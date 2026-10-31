#!/bin/bash
# Regtest stand for atomicswap.py: a mergemine bitcoind and a share chain freicoind in Docker,
# then stand.py runs the swap and both refunds against them.  Needs python3 on the host and a
# checkout of this repository (stand.py imports test/functional/test_framework).
#   BTC_IMAGE=freicoin/mergemine:v29.0.0.1 FRC_IMAGE=freicoin/freicoind:sharechain ./stand.sh
set -euo pipefail
BTC_IMAGE=${BTC_IMAGE:-freicoin/mergemine:v29.0.0.1}
FRC_IMAGE=${FRC_IMAGE:-freicoin/freicoind:sharechain}
BTC_PORT=${BTC_PORT:-18943}
FRC_PORT=${FRC_PORT:-18944}
USER=stand PASS=standpass
here=$(cd "$(dirname "$0")" && pwd)
cleanup() { docker rm -f atomicswap-btc atomicswap-frc >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup
docker run -d --rm --name atomicswap-btc -p 127.0.0.1:${BTC_PORT}:18443 --tmpfs /data:uid=1001 "$BTC_IMAGE" \
  -regtest -server -rpcbind=0.0.0.0 -rpcallowip=0.0.0.0/0 -rpcuser=$USER -rpcpassword=$PASS -fallbackfee=0.0001 -printtoconsole=0 >/dev/null
docker run -d --rm --name atomicswap-frc -p 127.0.0.1:${FRC_PORT}:28638 --tmpfs /data:uid=1002 "$FRC_IMAGE" \
  -regtest -server -rpcport=28638 -rpcbind=0.0.0.0 -rpcallowip=0.0.0.0/0 -rpcuser=$USER -rpcpassword=$PASS -fallbackfee=0.0001 -printtoconsole=0 >/dev/null
for i in $(seq 1 60); do
  if python3 -c "
import sys; sys.path.insert(0, '$here/..')
from common import Rpc
Rpc('http://127.0.0.1:$BTC_PORT', '$USER', '$PASS')('getblockcount'); Rpc('http://127.0.0.1:$FRC_PORT', '$USER', '$PASS')('getblockcount')" 2>/dev/null; then break; fi
  sleep 1
done
python3 "$here/stand.py" "http://127.0.0.1:$BTC_PORT" "http://127.0.0.1:$FRC_PORT" $USER $PASS
