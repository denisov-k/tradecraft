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
"""Test confidential scrip end to end: amounts hidden in Pedersen commitments with
range proofs in output witnesses, sent and unblinded by the wallet, the fee paid in
scrip, no freicoin needed."""

from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

CONFIDENTIAL = "-vbparams=confidential:-1:9223372036854775807"


class WalletConfidentialTest(FreicoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [[CONFIDENTIAL], [CONFIDENTIAL]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def confidential_outputs(self, node, txid):
        """The outputs whose extension push (after the witness program) is a confidential one: 0x43 (a
        67-byte push), then version 0x06."""
        def is_confidential(spk):
            ext = 4 + 2 * int(spk[2:4], 16)  # witness version, program push length, program
            return spk[ext:ext + 4] == "4306"
        return [o for o in node.getrawtransaction(txid, True)["vout"] if is_confidential(o["scriptPubKey"]["hex"])]

    def run_test(self):
        payer, holder = self.nodes
        self.generate(payer, 120)
        scrip = payer.convertscrip(0)["asset"]
        assert payer.getdeploymentinfo()["deployments"]["confidential"]["active"]

        self.log.info("Peg in: explicit scrip to start from")
        minted = payer.pegin(10)["scrip"]
        self.generate(payer, 1)
        assert_equal(payer.getassetbalances(), {scrip: minted})

        self.log.info("Send confidentially: the amount is in a commitment, its proof in the output witness")
        info = holder.getconfidentialaddress()
        res = payer.sendconfidential(info["address"], 100000, info["blindingkey"])
        outs = self.confidential_outputs(payer, res["txid"])
        assert_equal(len(outs), 2)  # the payment and the change
        for o in outs:
            assert_equal(o["value"], 0)
            assert_equal(len(o["witness"]), 1)  # the range proof
            assert len(o["witness"][0]) > 2 * 1000
        self.sync_mempools()
        self.generate(payer, 1)
        assert_equal(holder.listconfidential()["total"], 100000)
        assert_equal(payer.listconfidential()["total"], minted - 100000 - res["fee"])
        assert scrip not in payer.getassetbalances()  # its explicit scrip is spent

        self.log.info("Send back as explicit scrip, from confidential scrip alone: no freicoin")
        assert_equal(holder.getbalance(), 0)
        back = holder.sendconfidential(payer.getnewaddress(), 40000)
        self.sync_mempools()
        self.generate(payer, 1)
        assert_equal(payer.getassetbalances()[scrip], 40000)
        assert_equal(holder.listconfidential()["total"], 100000 - 40000 - back["fee"])

        self.log.info("Confidential to confidential, to a confidential address (the blinding key inside)")
        info0 = payer.getconfidentialaddress()
        assert info0["confidential"].startswith("cfcrt1")
        assert_raises_rpc_error(-8, "carries its blinding key already", holder.sendconfidential, info0["confidential"], 1, info0["blindingkey"])
        before = payer.listconfidential()["total"]
        res = holder.sendconfidential(info0["confidential"], 10000)
        self.sync_mempools()
        self.generate(payer, 1)
        assert_equal(payer.listconfidential()["total"], before + 10000)
        left = 100000 - 40000 - back["fee"] - 10000 - res["fee"]
        assert_equal(holder.listconfidential()["total"], left)

        self.log.info("The blinding keys survive a restart")
        self.restart_node(1, extra_args=[CONFIDENTIAL])
        self.connect_nodes(0, 1)
        assert_equal(self.nodes[1].listconfidential()["total"], left)

        self.log.info("Refusals")
        assert_raises_rpc_error(-6, "Insufficient scrip", self.nodes[1].sendconfidential, payer.getnewaddress(), left + 1, info0["blindingkey"])
        assert_raises_rpc_error(-8, "Invalid blinding key", self.nodes[1].sendconfidential, payer.getnewaddress(), 1, "02" + "00" * 32)


if __name__ == '__main__':
    WalletConfidentialTest(__file__).main()
