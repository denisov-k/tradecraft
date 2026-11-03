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
"""Test the scrip interface of the wallet RPCs (-scrip).

With -scrip the wallet RPCs express amounts in scrip, Freicoin valued at the
scrip epoch: an unspent amount holds constant while the chain grows, a payment
arrives exactly as sent, and the rounding of every conversion lands in the fee.
"""
from decimal import Decimal, ROUND_DOWN

from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

KRIA = Decimal('0.00000001')


class WalletScripTest(FreicoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 3
        self.extra_args = [[], ["-scrip"], ["-scrip"]]
        self.noban_tx_relay = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        native = self.nodes[0]
        self.nodes[1].createwallet("payer")
        payer = self.nodes[1].get_wallet_rpc("payer")
        self.nodes[2].createwallet("payee")
        payee = self.nodes[2].get_wallet_rpc("payee")

        self.log.info("A received amount is its scrip value, and holds constant")
        fund = (native.getbalance() / 4).quantize(KRIA, rounding=ROUND_DOWN)
        txid = native.sendtoaddress(payer.getnewaddress(), fund)
        self.sync_mempools()
        self.generate(native, 1)
        bal = payer.getbalance()
        # before the epoch scrip is the smaller number
        assert_greater_than(bal, 0)
        assert_greater_than(fund, bal)
        utxos = payer.listunspent()
        assert_equal(len(utxos), 1)
        assert_equal(utxos[0]["amount"], bal)
        assert_equal(utxos[0]["value"], bal)
        assert_equal(payer.gettransaction(txid)["amount"], bal)
        assert_equal(payer.getreceivedbyaddress(utxos[0]["address"]), bal)
        assert_equal([e["amount"] for e in payer.listtransactions()], [bal])
        self.generate(native, 150)
        assert_equal(payer.getbalance(), bal)
        assert_equal(payer.getbalances()["mine"]["trusted"], bal)
        assert_equal(payer.listunspent()[0]["amount"], bal)

        self.log.info("A payment arrives exactly as sent; the sender pays it plus the fee")
        amount = (bal / 5).quantize(KRIA, rounding=ROUND_DOWN)
        txid = payer.sendtoaddress(payee.getnewaddress(), amount)
        self.sync_mempools()
        self.generate(native, 1)
        assert_equal(payee.getbalance(), amount)
        assert_equal(payee.gettransaction(txid)["amount"], amount)
        assert_equal(payee.listunspent()[0]["amount"], amount)
        sent = payer.gettransaction(txid)
        fee = -sent["fee"]
        assert_greater_than(fee, 0)
        assert_equal(sent["amount"], -amount)
        assert_equal(sent["demurrage"], 0)
        assert_equal(payer.getbalance(), bal - amount - fee)
        send = [e for e in payer.listtransactions() if e["category"] == "send"]
        assert_equal(len(send), 1)
        assert_equal(send[0]["amount"], -amount)
        assert_equal(send[0]["fee"], -fee)
        assert_equal(send[0]["demurrage"], 0)
        left = payer.getbalance()

        self.log.info("Both sides hold constant as the chain grows")
        self.generate(native, 150)
        assert_equal(payee.getbalance(), amount)
        assert_equal(payer.getbalance(), left)

        self.log.info("sendmany: every output exact")
        small = 1000 * KRIA  # one kria of scrip is about a hundred of Freicoin, dust
        a, b = payee.getnewaddress(), payee.getnewaddress()
        txid = payer.sendmany("", {a: amount, b: small})
        self.sync_mempools()
        self.generate(native, 1)
        assert_equal(payee.getreceivedbyaddress(a), amount)
        assert_equal(payee.getreceivedbyaddress(b), small)
        assert_equal(payee.getbalance(), 2 * amount + small)
        assert_equal(payer.getbalance(), left - amount - small + payer.gettransaction(txid)["fee"])
        assert_equal(sorted(r["amount"] for r in payee.listreceivedbyaddress()), sorted([amount, amount, small]))

        self.log.info("RPCs that cannot speak scrip refuse to run")
        assert_raises_rpc_error(-4, "send does not express amounts in scrip", payer.send, {native.getnewaddress(): KRIA})
        assert_raises_rpc_error(-4, "sendall does not express amounts in scrip", payer.sendall, [native.getnewaddress()])
        assert_raises_rpc_error(-4, "listaddressgroupings does not express amounts in scrip", payer.listaddressgroupings)
        assert_raises_rpc_error(-8, "Amount filters are not available with -scrip", payer.listunspent, 0, 9999999, [], True, {"minimumAmount": 1})

        self.log.info("Without -scrip the same wallet shows Freicoin, the larger number")
        self.restart_node(2, extra_args=[])
        self.nodes[2].loadwallet("payee")
        payee = self.nodes[2].get_wallet_rpc("payee")
        assert_greater_than(payee.getbalance(), 10 * (2 * amount + small))


if __name__ == '__main__':
    WalletScripTest(__file__).main()
