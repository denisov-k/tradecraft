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
"""Test the wallet with user-issued assets: issueasset, sendasset,
getassetbalances, and that freicoin spending never touches asset outputs."""

from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class WalletAssetsTest(FreicoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 2
        self.noban_tx_relay = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        issuer, holder = self.nodes

        self.log.info("Issue an asset to a new address of the wallet")
        res = issuer.issueasset(1000)
        asset = res["asset"]
        self.sync_mempools()
        self.generate(issuer, 1)
        assert_equal(issuer.getassetbalances(), {asset: 1000})
        held = [u for u in issuer.listunspent() if "asset" in u]
        assert_equal(len(held), 1)
        assert_equal(held[0]["asset"], asset)
        assert_equal(held[0]["asset_amount"], 1000)
        assert_equal(held[0]["amount"], 0)
        assert_equal(issuer.gettransaction(res["txid"])["confirmations"], 1)

        self.log.info("Send part of it; the rest returns as asset change")
        to = holder.getnewaddress()
        txid = issuer.sendasset(asset, 300, to)
        self.sync_mempools()
        self.generate(issuer, 1)
        assert_equal(holder.getassetbalances(), {asset: 300})
        assert_equal(issuer.getassetbalances(), {asset: 700})
        assert_equal(holder.getreceivedbyaddress(to), 0)  # no freicoin came with it
        assert txid in [t["txid"] for t in holder.listtransactions()]

        self.log.info("Several outputs combine; the holder sends on")
        txid = issuer.sendasset(asset, 200, holder.getnewaddress())
        self.sync_mempools()
        self.generate(issuer, 1)
        assert_equal(holder.getassetbalances(), {asset: 500})
        holder.sendasset(asset, 450, issuer.getnewaddress())
        self.sync_mempools()
        self.generate(issuer, 1)
        assert_equal(holder.getassetbalances(), {asset: 50})
        assert_equal(issuer.getassetbalances(), {asset: 950})

        self.log.info("More than is held is refused")
        assert_raises_rpc_error(-6, "Insufficient asset", holder.sendasset, asset, 51, issuer.getnewaddress())
        assert_raises_rpc_error(-6, "Insufficient asset", holder.sendasset, "00" * 32, 1, issuer.getnewaddress())
        assert_raises_rpc_error(-3, "Amount out of range", holder.sendasset, asset, 0, issuer.getnewaddress())
        assert_raises_rpc_error(-8, "asset must be 32 bytes", holder.sendasset, "00", 1, issuer.getnewaddress())

        self.log.info("Two assets in one wallet")
        other = issuer.issueasset(5)["asset"]
        self.sync_mempools()
        self.generate(issuer, 1)
        assert_equal(issuer.getassetbalances(), {asset: 950, other: 5})

        self.log.info("Spending all the freicoin leaves the assets alone")
        issuer.sendall([holder.getnewaddress()])
        self.sync_mempools()
        self.generate(holder, 1)
        assert_equal(issuer.getassetbalances(), {asset: 950, other: 5})
        assert_equal(len([u for u in issuer.listunspent() if "asset" in u]), 3)


if __name__ == '__main__':
    WalletAssetsTest(__file__).main()
