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
"""Test user-issued assets on extension outputs (the assets deployment).

An asset output is a native witness output of zero value whose extension push
holds the asset id and amount.  A transaction issues the asset derived from
the outpoint its first input spends; otherwise, per asset, its outputs carry no
more than its inputs.  Checked in the mempool and in blocks.
"""
from decimal import Decimal
import hashlib
import struct

from test_framework.messages import CTxOut
from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.wallet import MiniWallet


def tagged_hash(tag, data):
    t = hashlib.sha256(tag.encode()).digest()
    return hashlib.sha256(t + t + data).digest()


def issuance_id(txid, vout):
    return tagged_hash("Freicoin/asset-issuance", bytes.fromhex(txid)[::-1] + struct.pack("<I", vout))


class AssetsTest(FreicoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def asset_out(self, asset, amount, value=0, ext=None):
        ext = ext if ext is not None else bytes([0x01]) + asset + struct.pack("<q", amount)
        return CTxOut(value, bytes(self.wallet.get_scriptPubKey()) + bytes([len(ext)]) + ext)

    def spend(self, utxos, outputs):
        """A transaction from the wallet's utxos (asset outputs included, OP_TRUE
        witnesses) paying the change to the wallet and then `outputs`."""
        tx = self.wallet.create_self_transfer_multi(utxos_to_spend=utxos, fee_per_output=1000000)["tx"]
        tx.vout += outputs
        tx.rehash()
        return tx

    def asset_utxo(self, tx, vout):
        return {"txid": tx.hash, "vout": vout, "value": Decimal(0), "refheight": tx.lock_height}

    def reject(self, tx, reason):
        res = self.node.testmempoolaccept([tx.serialize().hex()])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], reason)

    def run_test(self):
        self.node = self.nodes[0]
        self.wallet = MiniWallet(self.node)
        self.generate(self.wallet, 120)

        assert self.node.getdeploymentinfo()["deployments"]["assets"]["active"]

        self.log.info("Issue an asset: the id comes from the first input's outpoint")
        fund = self.wallet.get_utxo()
        asset = issuance_id(fund["txid"], fund["vout"])
        tx = self.spend([fund], [self.asset_out(asset, 1000), self.asset_out(asset, 500)])
        self.wallet.sendrawtransaction(from_node=self.node, tx_hex=tx.serialize().hex())
        self.generate(self.node, 1)
        issue = tx
        assert_equal(self.node.gettxout(issue.hash, 1)["value"], 0)

        self.log.info("An asset not derived from the first input cannot be issued")
        other = self.wallet.get_utxo()
        self.reject(self.spend([other], [self.asset_out(asset, 1)]), "bad-txns-asset-in-belowout")
        self.reject(self.spend([other], [self.asset_out(b"\x07" * 32, 1)]), "bad-txns-asset-in-belowout")

        self.log.info("Transfer: outputs may carry what the inputs hold, not more")
        self.reject(self.spend([other, self.asset_utxo(issue, 1)], [self.asset_out(asset, 700), self.asset_out(asset, 301)]),
                    "bad-txns-asset-in-belowout")
        tx = self.spend([other, self.asset_utxo(issue, 1)], [self.asset_out(asset, 700), self.asset_out(asset, 300)])
        self.wallet.sendrawtransaction(from_node=self.node, tx_hex=tx.serialize().hex())
        self.generate(self.node, 1)
        assert_equal(self.node.gettxout(tx.hash, 1)["value"], 0)

        self.log.info("Asset outputs hold no freicoin and are well formed")
        fee = self.wallet.get_utxo()
        self.reject(self.spend([fee, self.asset_utxo(issue, 2)], [self.asset_out(asset, 500, value=1000)]), "bad-txns-asset-nonzero-value")
        malformed = self.spend([fee, self.asset_utxo(issue, 2)], [self.asset_out(asset, 500, ext=bytes([0x01]) + asset + b"\x00" * 7)])
        self.reject(malformed, "dust")  # policy sees a zero-value output first
        assert_raises_rpc_error(None, "bad-txns-asset-malformed",
                                self.node.generateblock, self.wallet.get_descriptor(), [malformed.serialize().hex()])
        self.reject(self.spend([fee, self.asset_utxo(issue, 2)], [self.asset_out(asset, 0)]), "bad-txns-asset-amount-outofrange")

        self.log.info("A block carrying an inflating transfer is invalid")
        bad = self.spend([fee, self.asset_utxo(issue, 2)], [self.asset_out(asset, 501)])
        assert_raises_rpc_error(None, "bad-txns-asset-in-belowout",
                                self.node.generateblock, self.wallet.get_descriptor(), [bad.serialize().hex()])

        self.log.info("Burning is allowed: an asset input with no asset output")
        tx = self.spend([fee, self.asset_utxo(issue, 2)], [])
        self.wallet.sendrawtransaction(from_node=self.node, tx_hex=tx.serialize().hex())
        self.generate(self.node, 1)
        assert_equal(self.node.gettxout(issue.hash, 2), None)


if __name__ == '__main__':
    AssetsTest(__file__).main()
