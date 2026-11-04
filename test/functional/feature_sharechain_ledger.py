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
"""The share chain and the scrip ledger together: a block found through the share
chain pays a peg-out that fell due, and since it is also a share, the other node
must rebuild its coinbase with that payout and accept it."""

from feature_sharechain import StratumMiner, stratum_port
from test_framework.messages import COIN
from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import assert_equal


class ShareChainLedgerTest(FreicoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [[
            "-stratum=1",
            "-stratumbind=127.0.0.1",
            "-stratumport=%d" % stratum_port(i),
            "-stratumallowip=127.0.0.1",
            "-sharechain=regtest",
            "-miningmindiff=0.000001",
            "-debug=stratum",
        ] for i in range(2)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def state(self, node):
        i = node.getsharechaininfo()
        return (i["height"], i["tip"], i["chainshares"], i["fruits"])

    def run_test(self):
        n0, n1 = self.nodes
        self.generate(n0, 110)

        self.log.info("peg in, then peg out to node1")
        scrip = n0.pegin(10)["scrip"]
        self.generate(n0, 1)
        dest = n1.getnewaddress("", "bech32")
        res = n0.pegout(scrip // 2, dest)
        self.sync_mempools()

        self.log.info("mine through the share chain past the block that pays it")
        miner = StratumMiner(stratum_port(0), n0.getnewaddress("", "bech32"), self.log)
        payout_height = None
        while payout_height is None or n0.getblockcount() < payout_height:
            result, error = miner.mine_one()
            if error is not None:
                self.log.info("submit: %s", error)
            self.sync_blocks()
            if payout_height is None and res["txid"] in n0.getblock(n0.getbestblockhash())["tx"]:
                payout_height = n0.getblockcount() + 10
        block = n0.getblock(n0.getblockhash(payout_height), 2)
        expected = n0.convertscrip(scrip // 2, "freicoin", payout_height)["amount"]
        paid = [o for o in block["tx"][0]["vout"] if o["scriptPubKey"].get("address") == dest]
        assert_equal([int(o["value"] * COIN) for o in paid], [expected])

        self.log.info("the payout block is a share too: node1 rebuilt its coinbase and took it")
        self.wait_until(lambda: self.state(n0) == self.state(n1), timeout=60)
        assert n1.getsharechaininfo()["height"] >= 0
        assert not n1.getsharechaininfo().get("rejected"), n1.getsharechaininfo()
        miner.close()


if __name__ == '__main__':
    ShareChainLedgerTest(__file__).main()
