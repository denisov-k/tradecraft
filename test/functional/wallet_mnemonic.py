#!/usr/bin/env python3
# Copyright (c) 2026 The Freicoin developers
# Distributed under the terms of the GNU Affero General Public License v3.
"""Test 12-word (BIP39) recovery phrases: generatemnemonic, mnemonicdescriptors, importmnemonic.

Expected addresses were derived independently with the freicoin.ru web wallet's own code
(github.com/denisov-k/freicoin-wallet core/hd.mjs + @scure/bip39), regtest coin type 1."""

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

PHRASE = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
RECV = ["fcrt1qcvugv5smedss5kxgjk2lw40a2m5fv3pjct7dvm",
        "fcrt1qe82spt66l4gwa30aqxg4lpuqvngvkw0f0nfgqk",
        "fcrt1qgpjqlxdnm4kfjy94mchqatal5nvgxws270qjc0"]
CHANGE = ["fcrt1qw6slrem9m42hlkzlj77yn3lasz7aee08syfcgh",
          "fcrt1q0zmdks749qps0g5lw8fgv8ngdxqd2jadwh76rg",
          "fcrt1quju3mewerst250dxh4g3ujxsst7vfnkve3vsw9"]


class WalletMnemonicTest(FreicoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        # The test framework defaults to -keypool=1; a restore only finds coins
        # inside the keypool look-ahead, as with any HD wallet.
        self.extra_args = [["-keypool=100"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("generatemnemonic gives valid phrases of the requested length")
        for n in (12, 24):
            m = node.generatemnemonic(n)["mnemonic"]
            assert_equal(len(m.split()), n)
            node.mnemonicdescriptors(m)  # checksum must validate
        assert_equal(len(node.generatemnemonic()["mnemonic"].split()), 12)
        assert node.generatemnemonic()["mnemonic"] != node.generatemnemonic()["mnemonic"]
        assert_raises_rpc_error(-8, "words must be", node.generatemnemonic, 13)

        self.log.info("bad phrases are rejected")
        assert_raises_rpc_error(-8, "Invalid recovery phrase", node.mnemonicdescriptors, " ".join(["abandon"] * 12))
        assert_raises_rpc_error(-8, "Invalid recovery phrase", node.mnemonicdescriptors, PHRASE.replace("about", "zzzz"))
        assert_raises_rpc_error(-8, "Invalid recovery phrase", node.mnemonicdescriptors, " ".join(PHRASE.split()[:11]))

        self.log.info("addresses match the freicoin.ru wallet derivation")
        d = node.mnemonicdescriptors(PHRASE)
        assert d["receive"].startswith("wpk([")
        assert "/84h/1h/0h]" in d["receive"]
        assert_equal(node.deriveaddresses(d["receive"], [0, 2]), RECV)
        assert_equal(node.deriveaddresses(d["change"], [0, 2]), CHANGE)
        messy = "  " + PHRASE.upper().replace(" ", "   \n") + " "
        assert_equal(node.mnemonicdescriptors(messy), d)

        self.log.info("importmnemonic into a blank wallet makes those addresses")
        node.createwallet(wallet_name="w", blank=True)
        w = node.get_wallet_rpc("w")
        res = w.importmnemonic(PHRASE, "now")
        assert res["success"], res
        assert_equal(res["receive"], d["receive"])
        assert_equal(w.getnewaddress(), RECV[0])
        assert_equal(w.getnewaddress(), RECV[1])
        assert_equal(w.getrawchangeaddress(), CHANGE[0])
        assert_equal(w.getwalletinfo()["private_keys_enabled"], True)

        self.log.info("restore from the phrase with a full rescan finds the coins")
        self.generatetoaddress(node, 1, RECV[2])
        self.generatetoaddress(node, COINBASE_MATURITY, node.get_deterministic_priv_key().address)
        node.createwallet(wallet_name="restored", blank=True)
        r = node.get_wallet_rpc("restored")
        res = r.importmnemonic(PHRASE)  # default timestamp 0 = full rescan
        assert res["success"], res
        bal = r.getbalances()["mine"]["trusted"]
        assert bal > 0, bal
        assert_equal(w.getbalances()["mine"]["trusted"], bal)

        self.log.info("restored wallet can spend")
        txid = r.sendtoaddress(node.get_deterministic_priv_key().address, 1)
        assert txid in node.getrawmempool()

        self.log.info("numeric timestamp as a string works (cli style)")
        node.createwallet(wallet_name="ts", blank=True)
        assert node.get_wallet_rpc("ts").importmnemonic(PHRASE, "0")["success"]

        self.log.info("watch-only wallets are refused")
        node.createwallet(wallet_name="wo", disable_private_keys=True, blank=True)
        assert_raises_rpc_error(-4, "private keys enabled", node.get_wallet_rpc("wo").importmnemonic, PHRASE)


if __name__ == '__main__':
    WalletMnemonicTest(__file__).main()
