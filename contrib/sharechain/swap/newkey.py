#!/usr/bin/env python3
"""Make a single-key bech32 address outside the wallet, for a miner that will sell its
credits: prints `WIF ADDRESS`.  Uses the functional-test framework's pure-Python secp256k1
and Freicoin's P2WPK address derivation (test/functional/test_framework).

  newkey.py [main|regtest]      (default regtest)
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "test", "functional"))
from test_framework.address import key_to_p2wpk  # noqa: E402
from test_framework.key import ECKey  # noqa: E402
from test_framework.wallet_util import bytes_to_wif  # noqa: E402

main = len(sys.argv) > 1 and sys.argv[1] == "main"
key = ECKey()
key.generate(compressed=True)
wif = bytes_to_wif(key.get_bytes()) if not main else None
if main:
    # bytes_to_wif hardcodes the regtest/testnet prefix (239); mainnet is 128
    from test_framework.address import byte_to_base58
    wif = byte_to_base58(key.get_bytes() + b"\x01", 128)
print(wif, key_to_p2wpk(key.get_pubkey().get_bytes(), main=main))
