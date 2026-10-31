#!/bin/bash
# Apply the live-tested mergemine fixes from Vintage Hash (git format-patch against the 28.1 branch)
# to v29.0.0.1, plus our own -miningmindiff read-side rename.
#   0001 SubmitBlock copies cb/bf instead of moving them (aux share lost when the share is a BTC block)
#   0002 version-rolling.min-bit-count optional (ESP-Miner: Bitaxe, NerdQAxe, Zyber)
#   0003 resend work after 60 s idle (cgminer / ESP-Miner keepalive)
#   0004 -walletblockfinaltx=<wallet name> no longer read as a boolean
#   0007 mining.subscribe accepts up to 4 parameters (Braiins OS / BOSminer); context differs on
#        v29, so it is applied with fuzz and committed under the same author
# Skipped: 0005 (-stratumbind help text: wording differs on v29), 0006 (registers
# -miningmindifficulty; we rename the read side to the registered -miningmindiff instead).
set -e
git config user.email build@freicoin.ru
git config user.name "freicoin.ru build"
for p in 0001 0002 0003 0004; do
  f=$(ls /tmp/patches/${p}-*.patch)
  echo "== applying $(basename "$f")"
  git am "$f"
done
f7=$(ls /tmp/patches/0007-*.patch)
echo "== applying $(basename "$f7") with fuzz"
if ! patch -p1 -F3 --no-backup-if-mismatch < "$f7"; then
  echo "== fuzz failed, applying the change by hand"
  sed -i '/^UniValue stratum_mining_subscribe/,/^}/ s/BoundParams(method, params, 0, 2);/BoundParams(method, params, 0, 4); \/\/ Braiins OS sends pool host and port too/' src/stratum.cpp
fi
git commit -q -a --author="Vintage Hash <vintagehash@users.noreply.github.com>" -m "stratum: accept extra mining.subscribe parameters

Ported to v29.0.0.1 from the 28.1 patch: Braiins OS (BOSminer) sends four
parameters (adds pool host and port) and was refused on every connect."
f=src/stratum.cpp
sed -i 's/GetArg("-miningmindifficulty")/GetArg("-miningmindiff")/; s/Invalid -miningmindifficulty=/Invalid -miningmindiff=/' $f
grep -n 'MakeTransactionRef(cb)\|MakeTransactionRef(bf)\|"-miningmindiff"\|keepalive\|BoundParams(method, params, 0, 4)' $f
git log --oneline -6
