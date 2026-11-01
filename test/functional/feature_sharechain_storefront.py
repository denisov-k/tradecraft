#!/usr/bin/env python3
# Copyright (c) 2026 The Freicoin Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php.
"""The market maker's storefront API (contrib/sharechain/swap/maker.py), as a wallet page uses
it: quotes for the wallet's addresses, an unsigned locked transfer built by the maker, signed
by the seller's key outside the maker, submitted, and followed until the maker has claimed it.
Trust mode (no lightning): the seller hands over the preimage with the signed transfer."""
import hashlib
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from urllib.parse import urlparse

from feature_sharechain import StratumMiner
from test_framework.address import key_to_p2wpk
from test_framework.key import ECKey
from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import assert_equal, stratum_port

MAKER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "contrib", "sharechain", "swap", "maker.py")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class ShareChainStorefrontTest(FreicoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-stratum=1",
            "-stratumbind=127.0.0.1",
            "-stratumport=%d" % stratum_port(0),
            "-stratumallowip=127.0.0.1",
            "-sharechain=regtest",
            "-miningmindiff=0.000001",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def http(self, path, payload=None):
        data = json.dumps(payload).encode() if payload is not None else None
        req = urllib.request.Request(self.maker_url + path, data, {"Content-Type": "application/json"} if data else {})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, json.load(r)
        except urllib.error.HTTPError as e:
            return e.code, json.load(e)

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 110)
        maker_addr = node.getnewaddress("", "bech32")
        key = ECKey()
        key.generate()
        pub = key.get_pubkey().get_bytes()
        seller = key_to_p2wpk(pub)

        self.log.info("mine credits to the seller's address")
        m = StratumMiner(stratum_port(0), seller, self.log)
        owners = lambda: {c["address"]: c for c in node.getsharecredits()["credits"]}
        while owners().get(seller, {}).get("fruits", 0) < 3:
            assert_equal(m.mine_one()[1], None)

        self.log.info("start the maker (trust mode)")
        u = urlparse(node.url)
        port = free_port()
        self.maker_url = f"http://127.0.0.1:{port}"
        proc = subprocess.Popen([sys.executable, MAKER, "--rpc", f"http://127.0.0.1:{u.port}", "--rpcuser", u.username,
                                 "--rpcpassword", u.password, "--address", maker_addr, "--sats-per-unit", "1000000000000",
                                 "--min-sats", "0", "--expiry", "40", "--min-left", "3", "--no-lightning",
                                 "--listen", f"127.0.0.1:{port}"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            self.wait_until(lambda: self.http_ok("/offers"), timeout=30)

            self.log.info("quotes for the wallet's addresses: only those with credit come back")
            other = ECKey()
            other.generate()
            code, quotes = self.http("/quotes?addresses=" + seller + "," + key_to_p2wpk(other.get_pubkey().get_bytes()))
            assert_equal(code, 200)
            assert_equal(list(quotes), [seller])

            self.log.info("prepare: the maker builds the locked transfer; a key that is not the address's is refused")
            preimage = os.urandom(32)
            hashlock = hashlib.sha256(preimage).hexdigest()
            code, err = self.http("/prepare", {"address": seller, "pubkey": other.get_pubkey().get_bytes().hex(), "hashlock": hashlock})
            assert_equal(code, 400)
            assert "not this address's" in err["error"], err
            code, prep = self.http("/prepare", {"address": seller, "pubkey": pub.hex(), "hashlock": hashlock})
            assert_equal(code, 200)
            assert_equal(prep["from"], seller)

            self.log.info("the seller signs outside the maker and submits; a bad signature is refused")
            code, err = self.http("/submit", {"hex": prep["hex"], "signature": other.sign_ecdsa(bytes.fromhex(prep["sighash"])).hex(), "preimage": preimage.hex()})
            assert code in (400, 500), (code, err)
            sig = key.sign_ecdsa(bytes.fromhex(prep["sighash"]))
            code, sub = self.http("/submit", {"hex": prep["hex"], "signature": sig.hex(), "preimage": preimage.hex()})
            assert_equal(code, 200)
            txid = sub["txid"]
            assert_equal(self.http("/status?txid=" + txid)[1]["state"], "waiting")

            self.log.info("follow it: carried, locked, claimed by the maker")
            deadline = time.time() + 120
            while self.http("/status?txid=" + txid)[1]["state"] != "paid":
                assert time.time() < deadline, self.http("/status?txid=" + txid)
                assert_equal(m.mine_one()[1], None)
            # "paid" is the seller's moment (the maker has paid and sent its claim); the credit
            # moves to the maker once a chain share carries the claim
            while maker_addr not in owners():
                assert time.time() < deadline, node.listsharetxs()
                assert_equal(m.mine_one()[1], None)
            code, feed = self.http("/offers")
            assert_equal(feed["sales"][-1]["transfer"], txid)
            assert_equal(feed["sales"][-1]["seller"], seller)
        finally:
            m.close()
            proc.terminate()
            out = proc.communicate(timeout=10)[0]
            self.log.debug("maker output:\n" + out)

    def http_ok(self, path):
        try:
            return self.http(path)[0] == 200
        except Exception:
            return False


if __name__ == "__main__":
    ShareChainStorefrontTest(__file__).main()
