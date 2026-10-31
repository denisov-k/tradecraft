#!/usr/bin/env python3
# Copyright (c) 2026 The Freicoin Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php.
"""Sell share chain credits for FRC through an on-chain HTLC (contrib/sharechain/swap/frchtlc.py,
doc/sharechain-transfers.md §4.2): the maker funds the HTLC, the seller locks credits under its
hashlock, the maker's claim reveals the preimage, the seller spends the HTLC with it.  Then the
refund path: an offer nobody takes is refunded only after its timeout."""
import os
import sys
from decimal import Decimal

from feature_sharechain import StratumMiner
from test_framework.address import key_to_p2wpk
from test_framework.key import ECKey
from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, stratum_port
from test_framework.wallet_util import bytes_to_wif

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "contrib", "sharechain", "swap"))
import frchtlc  # noqa: E402


class ShareChainFrcSwapTest(FreicoinTestFramework):
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

    def run_test(self):
        node = self.nodes[0]

        def rpc(method, *params):
            return getattr(node, method)(*params)

        self.log.info("bootstrap: spendable FRC for the maker, credits for the seller")
        self.generate(node, 110)
        maker_credits = node.getnewaddress("", "bech32")
        key = ECKey()
        key.generate()
        seller = key_to_p2wpk(key.get_pubkey().get_bytes())
        wif = bytes_to_wif(key.get_bytes())
        owners = lambda: {c["address"]: c for c in node.getsharecredits()["credits"]}
        m = StratumMiner(stratum_port(0), seller, self.log)
        while owners().get(seller, {}).get("fruits", 0) < 4:
            result, error = m.mine_one()
            assert_equal(error, None)
        sold_work = owners()[seller]["work"]

        self.log.info("maker: fund the HTLC")
        # regtest shares are often blocks too: leave room for the mining below
        offer, state = frchtlc.make_offer(rpc, key.get_pubkey().get_bytes(), Decimal("1.5"), 60, 0, maker_credits)
        self.generate(node, 1)

        self.log.info("seller: check the offer, lock the credits under its hashlock")
        frchtlc.check_offer(rpc, offer, key, min_blocks=5, min_conf=1)
        other = ECKey()
        other.generate()
        try:
            frchtlc.check_offer(rpc, offer, other, min_blocks=5, min_conf=1)
            raise AssertionError("an offer for another key passed")
        except RuntimeError as e:
            assert "not the HTLC for our key" in str(e)
        transfer = frchtlc.lock_credits(rpc, offer, wif, 30)
        while not frchtlc.find_locked(rpc, state):
            result, error = m.mine_one()
            assert_equal(error, None)
        assert_equal(frchtlc.revealed_preimage(rpc, transfer, offer["hashlock"]), None)

        self.log.info("maker: claim the credits, which reveals the preimage")
        claim, err = frchtlc.claim_credits(rpc, state, min_left=1)
        assert_equal(err, None)
        while frchtlc.find_locked(rpc, state):
            result, error = m.mine_one()
            assert_equal(error, None)
        assert owners()[maker_credits]["work"] >= sold_work
        assert_equal(frchtlc.revealed_preimage(rpc, transfer, offer["hashlock"]), state["preimage"])
        assert_equal(node.listsharetxs()["claimed"][0]["txid"], claim)

        self.log.info("seller: spend the HTLC with the preimage")
        frc_to = node.getnewaddress("", "bech32")
        raw = frchtlc.spend_htlc(rpc, frchtlc.CScript(bytes.fromhex(offer["script"])), offer["txid"], offer["vout"],
                                 key, frc_to, preimage=bytes.fromhex(state["preimage"]))
        # a wrong preimage does not spend it
        bad = frchtlc.spend_htlc(rpc, frchtlc.CScript(bytes.fromhex(offer["script"])), offer["txid"], offer["vout"],
                                 key, frc_to, preimage=bytes(32))
        assert_raises_rpc_error(-26, None, node.sendrawtransaction, bad)
        txid = node.sendrawtransaction(raw)
        self.generate(node, 1)
        got = node.gettxout(txid, 0)
        assert got and Decimal(str(got["value"])) > Decimal("1.49"), got
        assert_equal(node.gettxout(offer["txid"], offer["vout"]), None)
        m.close()

        self.log.info("an offer nobody takes is refunded, and only after its timeout")
        offer2, state2 = frchtlc.make_offer(rpc, key.get_pubkey().get_bytes(), Decimal("0.7"), 3, 0, maker_credits)
        self.generate(node, 1)
        refund_to = node.getnewaddress("", "bech32")
        script2 = frchtlc.CScript(bytes.fromhex(state2["script"]))
        early = frchtlc.spend_htlc(rpc, script2, state2["txid"], state2["vout"], frchtlc.wif_to_key(state2["refundwif"]),
                                   refund_to, timeout=state2["timeout"])
        assert_raises_rpc_error(-26, None, node.sendrawtransaction, early)
        while node.getblockcount() < state2["timeout"]:
            self.generate(node, 1)
        late = frchtlc.spend_htlc(rpc, script2, state2["txid"], state2["vout"], frchtlc.wif_to_key(state2["refundwif"]),
                                  refund_to, timeout=state2["timeout"])
        rtxid = node.sendrawtransaction(late)
        self.generate(node, 1)
        assert Decimal(str(node.gettxout(rtxid, 0)["value"])) > Decimal("0.69")


if __name__ == "__main__":
    ShareChainFrcSwapTest(__file__).main()
