#!/usr/bin/env python3
# Copyright (c) 2026 The Freicoin Developers
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of version 3 of the GNU Affero General Public License as published
# by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more
# details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Test the wallet with the scrip ledger: pegin, scrip as an asset, pegout,
and the ledger in getblocktemplate."""
from decimal import Decimal

from test_framework.messages import COIN
from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

RULES = {"rules": ["segwit", "finaltx", "auxpow", "assets"]}


class WalletScripLedgerTest(FreicoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 2
        self.noban_tx_relay = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        payer, holder = self.nodes
        scrip = payer.convertscrip(0)["asset"]

        self.log.info("pegin: freicoin into the pool, scrip out")
        res = payer.pegin(10)
        minted = res["scrip"]
        assert_equal(minted, payer.convertscrip(10 * COIN, "scrip", payer.getblockcount() + 1)["amount"])
        self.sync_mempools()
        self.generate(payer, 1)
        assert_equal(payer.getassetbalances(), {scrip: minted})
        tmpl = payer.getblocktemplate(RULES)
        pool = tmpl["finaltx"]["pool"]
        assert_greater_than(pool["amount"], 10 * COIN - 10 * COIN // 1000)
        assert pool["scriptPubKey"].startswith("0002465309")

        self.log.info("scrip moves like any asset")
        half = minted // 2
        payer.sendasset(scrip, half, holder.getnewaddress())
        self.sync_mempools()
        self.generate(payer, 1)
        assert_equal(holder.getassetbalances(), {scrip: half})
        assert_equal(payer.getassetbalances(), {scrip: minted - half})

        self.log.info("pegout: scrip burned, freicoin paid by the coinbase")
        dest = holder.getnewaddress()
        out = half // 3
        res = holder.pegout(out, dest)
        # paid by the coinbase ledger_payout_delay (10 on regtest) blocks after it confirms
        assert_equal(res["payout_height"], holder.getblockcount() + 1 + 10)
        expected = holder.convertscrip(out, "freicoin", res["payout_height"])["amount"]
        assert_equal(res["freicoin"], Decimal(expected) / COIN)
        self.sync_mempools()
        self.generate(holder, 10)
        tmpl = holder.getblocktemplate(RULES)
        assert_equal(tmpl["coinbasepayouts"], [{"scriptPubKey": holder.getaddressinfo(dest)["scriptPubKey"], "amount": expected}])
        block = holder.getblock(self.generate(holder, 1)[0], 2)
        assert_equal(block["height"], res["payout_height"])
        paid = [o for o in block["tx"][0]["vout"] if o["scriptPubKey"].get("address") == dest]
        assert_equal([int(o["value"] * COIN) for o in paid], [expected])
        assert_equal(holder.getassetbalances(), {scrip: half - out})
        # the peg-out itself is nothing the wallet can spend
        assert not [u for u in holder.listunspent() if u["address"] == dest and u["amount"] == 0]

        self.log.info("More than is held is refused")
        assert_raises_rpc_error(-6, "Insufficient asset", holder.pegout, half, dest)
        assert_raises_rpc_error(-3, "Amount out of range", holder.pegout, 0, dest)


if __name__ == '__main__':
    WalletScripLedgerTest(__file__).main()
