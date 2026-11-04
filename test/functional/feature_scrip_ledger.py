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
"""Test the scrip ledger: freicoin paid into the pool of the block-final
transaction mints scrip, an asset without demurrage; scrip paid out is paid
in freicoin by the coinbase of the same block."""
from decimal import Decimal
import struct

from test_framework.messages import COIN, CTxOut
from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error
from test_framework.wallet import MiniWallet

LEDGER_BASE = bytes([0x00, 0x02]) + b"FS"


def ledger_ext(version, amount):
    ext = bytes([version]) + struct.pack("<q", amount)
    return bytes([len(ext)]) + ext


class ScripLedgerTest(FreicoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def scrip_out(self, amount, base=None):
        ext = bytes([0x01]) + self.scrip + struct.pack("<q", amount)
        return CTxOut(0, (base or bytes(self.wallet.get_scriptPubKey())) + bytes([len(ext)]) + ext)

    def spend(self, utxos, outputs, pay_in=0):
        tx = self.wallet.create_self_transfer_multi(utxos_to_spend=utxos, fee_per_output=1000000)["tx"]
        tx.vout[0].nValue -= pay_in
        tx.vout += outputs
        tx.rehash()
        return tx

    def fee_utxo(self):
        """The wallet's oldest coin: MiniWallet hands out coinbases at exactly maturity,
        which the reorg below would make immature again."""
        oldest = min(self.wallet.get_utxos(mark_as_spent=False), key=lambda u: u["height"] if u["height"] > 0 else 1 << 30)
        return self.wallet.get_utxo(txid=oldest["txid"], vout=oldest["vout"])

    def utxo(self, tx, vout, value=0):
        return {"txid": tx.hash, "vout": vout, "value": Decimal(value) / COIN, "refheight": tx.lock_height}

    def reject(self, tx, reason):
        res = self.node.testmempoolaccept([tx.serialize().hex()])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], reason)

    def send(self, tx):
        self.wallet.sendrawtransaction(from_node=self.node, tx_hex=tx.serialize().hex())

    def final_pool(self, block):
        """The pool of a block: the first output of its block-final transaction."""
        out = block["tx"][-1]["vout"][0]
        script = bytes.fromhex(out["scriptPubKey"]["hex"])
        assert script.startswith(LEDGER_BASE + bytes([9, 0x02])), script.hex()
        return int(out["value"] * COIN), struct.unpack("<q", script[6:])[0]

    def run_test(self):
        self.node = self.nodes[0]
        self.wallet = MiniWallet(self.node)
        self.generate(self.wallet, 130)
        self.scrip = bytes.fromhex(self.node.convertscrip(0)["asset"])

        self.log.info("Peg in: freicoin paid to the pool mints its scrip value, no more")
        fund = self.fee_utxo()
        pay_in = int(fund["value"] * COIN) // 2
        tx = self.spend([fund], [], pay_in=pay_in)
        minted = self.node.convertscrip(pay_in, "scrip", tx.lock_height)["amount"]
        assert_greater_than(minted, 0)
        pegin = CTxOut(pay_in, LEDGER_BASE + ledger_ext(0x03, 0))
        self.reject(self.spend([fund], [pegin, self.scrip_out(minted + 1)], pay_in=pay_in), "bad-txns-asset-in-belowout")
        tx = self.spend([fund], [pegin, self.scrip_out(minted)], pay_in=pay_in)
        self.send(tx)
        block = self.node.getblock(self.generate(self.node, 1)[0], 2)
        pool_value, pool_scrip = self.final_pool(block)
        assert_equal(pool_scrip, minted)
        assert pay_in - pay_in // 1000 < pool_value <= pay_in, (pool_value, pay_in)
        holding = self.utxo(tx, 2)
        pegin1_hash = block["hash"]

        self.log.info("The pool and peg-ins are not anyone's to spend")
        fee = self.fee_utxo()
        pending = self.spend([fee], [pegin, self.scrip_out(1)], pay_in=pay_in // 4)
        pending.vout[1].nValue = pay_in // 4
        pending.rehash()
        self.send(pending)
        # the pool records what a peg-in may mint; what it leaves unminted never exists
        minted2 = self.node.convertscrip(pay_in // 4, "scrip", pending.lock_height)["amount"]
        self.reject(self.spend([self.fee_utxo(), self.utxo(pending, 1, pay_in // 4)], []), "bad-txns-ledger-spend")
        block = self.node.getblock(self.generate(self.node, 1)[0], 2)
        pool_value2, pool_scrip2 = self.final_pool(block)
        pegin2_hash = block["hash"]
        assert_equal(pool_scrip2, minted + minted2)
        assert_greater_than(pool_value2, pool_value)

        self.log.info("Peg out: scrip burned, its freicoin value paid by the coinbase")
        dest = bytes(self.wallet.get_scriptPubKey())
        part = minted // 3
        fee = self.fee_utxo()
        self.reject(self.spend([fee, holding], [CTxOut(0, dest + ledger_ext(0x04, minted + 1))]), "bad-txns-asset-in-belowout")
        self.reject(self.spend([fee, holding], [CTxOut(1000, dest + ledger_ext(0x04, part))]), "bad-txns-pegout-nonzero-value")
        tx = self.spend([fee, holding], [CTxOut(0, dest + ledger_ext(0x04, part)), self.scrip_out(minted - part)])
        self.send(tx)
        height = self.node.getblockcount() + 1
        block = self.node.getblock(self.generate(self.node, 1)[0], 2)
        expected = self.node.convertscrip(part, "freicoin", height)["amount"]
        payouts = [o for o in block["tx"][0]["vout"] if o["scriptPubKey"]["hex"] == dest.hex()]
        assert_equal([int(o["value"] * COIN) for o in payouts], [expected])
        _, pool_scrip3 = self.final_pool(block)
        assert_equal(pool_scrip3, minted + minted2 - part)
        out1 = tx
        self.reject(self.spend([self.fee_utxo(), self.utxo(tx, 1)], []), "bad-txns-pegout-spend")

        self.log.info("Peg out the rest of the first peg-in")
        rest = self.utxo(tx, 2)
        fee = self.fee_utxo()
        tx = self.spend([fee, rest], [CTxOut(0, dest + ledger_ext(0x04, minted - part))])
        self.send(tx)
        block = self.node.getblock(self.generate(self.node, 1)[0], 2)
        _, pool_scrip4 = self.final_pool(block)
        assert_equal(pool_scrip4, minted2)  # what the second peg-in may still account for
        out2 = tx

        self.log.info("Reorg: the ledger blocks undone, their transactions back, the same ledger again")
        self.node.invalidateblock(pegin2_hash)
        assert_equal(self.node.getbestblockhash(), pegin1_hash)
        mempool = self.node.getrawmempool()
        back = {"the second peg-in": pending, "the peg-out": out1, "the peg-out of the rest": out2}
        missing = {name: self.node.testmempoolaccept([tx.serialize().hex()])[0] for name, tx in back.items() if tx.hash not in mempool}
        assert not missing, missing
        assert_equal(len(mempool), 3)
        height = self.node.getblockcount() + 1
        expected = sorted([self.node.convertscrip(part, "freicoin", height)["amount"],
                           self.node.convertscrip(minted - part, "freicoin", height)["amount"]])
        assert_raises_rpc_error(-8, "Support for 'assets' rule", self.node.getblocktemplate, {"rules": ["segwit", "finaltx", "auxpow"]})
        tmpl = self.node.getblocktemplate({"rules": ["segwit", "finaltx", "auxpow", "assets"]})
        assert_equal(sorted(p["amount"] for p in tmpl["coinbasepayouts"]), expected)
        assert_equal(struct.unpack("<q", bytes.fromhex(tmpl["finaltx"]["pool"]["scriptPubKey"])[6:])[0], minted2)
        block = self.node.getblock(self.generate(self.node, 1)[0], 2)
        assert {pending.hash, out1.hash, out2.hash} <= {t["txid"] for t in block["tx"]}
        payouts = [o for o in block["tx"][0]["vout"] if o["scriptPubKey"]["hex"] == dest.hex()]
        assert_equal(sorted(int(o["value"] * COIN) for o in payouts), expected)
        _, pool_scrip5 = self.final_pool(block)
        assert_equal(pool_scrip5, minted2)
        assert self.node.gettxout(block["tx"][-1]["txid"], 0) is not None
        assert_equal(self.node.getrawmempool(), [])


if __name__ == '__main__':
    ScripLedgerTest(__file__).main()
