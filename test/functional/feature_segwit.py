#!/usr/bin/env python3
# Copyright (c) 2016-2022 The Bitcoin Core developers
# Copyright (c) 2010-2024 The Freicoin Developers
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
"""Test the SegWit changeover logic."""

from decimal import Decimal

from test_framework.address import (
    script_to_p2wsh,
)
from test_framework.blocktools import (
    send_to_witness,
)
from test_framework.descriptors import descsum_create
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    fastHash256,
    hash256,
    tx_from_hex,
)
from test_framework.script import (
    CScript,
    OP_DROP,
    OP_TRUE,
    ripemd160,
)
from test_framework.script_util import (
    key_to_p2pk_script,
    key_to_p2wpk_script,
    keys_to_multisig_script,
    script_to_p2sh_script,
    script_to_p2wsh_script,
    script_to_p2wpk_script,
)
from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than_or_equal,
    assert_raises_rpc_error,
)
from test_framework.wallet_util import (
    get_generate_key,
)

NODE_0 = 0
NODE_2 = 2
P2WPK = 0
P2WSH = 1


def getutxo(txid):
    utxo = {}
    utxo["vout"] = 0
    utxo["txid"] = txid
    return utxo


def find_spendable_utxo(node, min_value):
    for utxo in node.listunspent(query_options={'minimumAmount': min_value}):
        if utxo['spendable']:
            return utxo

    raise AssertionError(f"Unspent output equal or higher than {min_value} not found")


txs_mined = {}  # txindex from txid to blockhash


class SegWitTest(FreicoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        # This test tests SegWit both pre and post-activation, so use the normal BIP9 activation.
        self.extra_args = [
            [
                "-acceptnonstdtxn=1",
                "-testactivationheight=segwit@165",
                "-addresstype=legacy",
            ],
            [
                "-acceptnonstdtxn=1",
                "-testactivationheight=segwit@165",
                "-addresstype=legacy",
            ],
            [
                "-acceptnonstdtxn=1",
                "-testactivationheight=segwit@165",
                "-addresstype=legacy",
            ],
        ]
        self.rpc_timeout = 120

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        super().setup_network()
        self.connect_nodes(0, 2)
        self.sync_all()

    def success_mine(self, node, txid, sign, redeem_script=""):
        send_to_witness(1, node, getutxo(txid), self.pubkey[0], Decimal("49.998"), sign, redeem_script)
        block = self.generate(node, 1)
        assert_equal(len(node.getblock(block[0])["tx"]), 3)
        self.sync_blocks()

    def fail_accept(self, node, error_msg, txid, sign, redeem_script=""):
        assert_raises_rpc_error(-26, error_msg, send_to_witness, use_p2wsh=1, node=node, utxo=getutxo(txid), pubkey=self.pubkey[0], amount=Decimal("49.998"), sign=sign, insert_redeem_script=redeem_script)

    def run_test(self):
        self.generate(self.nodes[0], 161)  # block 161

        self.log.info("Verify sigops are counted in GBT with pre-BIP141 rules before the fork")
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        tmpl = self.nodes[0].getblocktemplate({'rules': ['segwit','finaltx','auxpow']})
        assert_equal(tmpl['sizelimit'], 1000000)
        assert 'weightlimit' not in tmpl
        assert_equal(tmpl['sigoplimit'], 20000)
        assert_equal(tmpl['transactions'][0]['hash'], txid)
        assert_equal(tmpl['transactions'][0]['sigops'], 2)
        assert '!segwit' not in tmpl['rules']
        self.generate(self.nodes[0], 1)  # block 162

        balance_presetup = self.nodes[0].getbalance()
        self.pubkey = []
        wit_ids = []  # wit_ids[NODE][TYPE] is an array of txids that spend to P2WPK (TYPE=0) or P2WSH (TYPE=1) scripts to an address for NODE via bare witness
        for i in range(3):
            key = get_generate_key()
            self.pubkey.append(key.pubkey)

            multiscript = keys_to_multisig_script([self.pubkey[-1]])
            bip173_ms_addr = self.nodes[i].createmultisig(1, [self.pubkey[-1]], 'bech32')['address']
            assert_equal(bip173_ms_addr, script_to_p2wsh(multiscript))

            bip173_ms_desc = descsum_create(f"wsh(multi(1,{key.privkey}))")
            assert_equal(self.nodes[i].deriveaddresses(bip173_ms_desc)[0], bip173_ms_addr)

            wpk_desc = descsum_create(f"wpk({key.privkey})")
            assert_equal(self.nodes[i].deriveaddresses(wpk_desc)[0], key.p2wpk_addr)

            res = self.nodes[i].importdescriptors([
                {"desc": bip173_ms_desc, "timestamp": "now"},
                {"desc": wpk_desc, "timestamp": "now"},
            ])
            assert all([r["success"] for r in res])

            wit_ids.append([])
            for _ in range(2):
                wit_ids[i].append([])

        for _ in range(5):
            for n in range(3):
                for v in range(2):
                    wit_ids[n][v].append(send_to_witness(v, self.nodes[0], find_spendable_utxo(self.nodes[0], 50), self.pubkey[n], Decimal("49.999")))

        self.generate(self.nodes[0], 1)  # block 163

        # Make sure all nodes recognize the transactions as theirs
        assert_equal(self.nodes[0].getbalance(), balance_presetup - 30 * 50 + 10 * Decimal("49.999") + 50)
        assert_equal(self.nodes[1].getbalance(), 10 * Decimal("49.999"))
        assert_equal(self.nodes[2].getbalance(), 10 * Decimal("49.999"))

        self.generate(self.nodes[0], 1)  # block 164

        self.log.info("Verify witness txs are mined as soon as segwit activates")

        send_to_witness(1, self.nodes[2], getutxo(wit_ids[NODE_2][P2WPK][0]), self.pubkey[0], amount=Decimal("49.998"), sign=True)
        send_to_witness(1, self.nodes[2], getutxo(wit_ids[NODE_2][P2WSH][0]), self.pubkey[0], amount=Decimal("49.998"), sign=True)

        assert_equal(len(self.nodes[2].getrawmempool()), 2)
        blockhash = self.generate(self.nodes[2], 1)[0]  # block 165 (first block with new rules)
        assert_equal(len(self.nodes[2].getrawmempool()), 0)
        segwit_tx_list = self.nodes[2].getblock(blockhash)["tx"][:-1]
        assert_equal(len(segwit_tx_list), 3)

        self.log.info("Verify default node can't accept txs with missing witness")
        # unsigned, no scriptsig
        self.fail_accept(self.nodes[0], "mempool-script-verify-flag-failed (Witness program was passed an empty witness)", wit_ids[NODE_0][P2WPK][0], sign=False)
        self.fail_accept(self.nodes[0], "mempool-script-verify-flag-failed (Witness program was passed an empty witness)", wit_ids[NODE_0][P2WSH][0], sign=False)

        # Coinbase contains the witness commitment nonce, check that RPC shows us
        coinbase_txid = self.nodes[2].getblock(blockhash)['tx'][0]
        coinbase_tx = self.nodes[2].gettransaction(txid=coinbase_txid, verbose=True)
        witnesses = coinbase_tx["decoded"]["vin"][0]["txinwitness"]
        assert_equal(len(witnesses), 1)
        assert_equal(witnesses[0], '')

        self.log.info("Verify witness txs without witness data are invalid after the fork")
        self.fail_accept(self.nodes[2], 'mempool-script-verify-flag-failed (Witness program was passed an empty witness)', wit_ids[NODE_2][P2WPK][2], sign=False)
        self.fail_accept(self.nodes[2], 'mempool-script-verify-flag-failed (Witness program was passed an empty witness)', wit_ids[NODE_2][P2WSH][2], sign=False)

        self.log.info("Verify default node can now use witness txs")
        self.success_mine(self.nodes[0], wit_ids[NODE_0][P2WPK][0], True)
        self.success_mine(self.nodes[0], wit_ids[NODE_0][P2WSH][0], True)
        self.generate(self.nodes[0], 1)
        self.generate(self.nodes[0], 1)

        self.log.info("Verify sigops are counted in GBT with BIP141 rules after the fork")
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        raw_tx = self.nodes[0].getrawtransaction(txid, True)
        tmpl = self.nodes[0].getblocktemplate({'rules': ['segwit','finaltx','auxpow']})
        assert_greater_than_or_equal(tmpl['sizelimit'], 3999577)  # actual maximum size is lower due to minimum mandatory non-witness data
        assert_equal(tmpl['weightlimit'], 4000000)
        assert_equal(tmpl['sigoplimit'], 80000)
        assert_equal(tmpl['transactions'][0]['txid'], txid)
        assert_equal(tmpl['transactions'][0]['sigops'], 8)
        assert '!segwit' in tmpl['rules']

        self.generate(self.nodes[0], 1)  # Mine a block to clear the gbt cache

        self.log.info("Non-segwit miners are able to use GBT response after activation.")
        # Create a 3-tx chain: tx1 (non-segwit input, paying to a segwit output) ->
        #                      tx2 (segwit input, paying to a non-segwit output) ->
        #                      tx3 (non-segwit input, paying to a non-segwit output).
        # tx1 is allowed to appear in the block, but no others.
        txid1 = send_to_witness(1, self.nodes[0], find_spendable_utxo(self.nodes[0], 50), self.pubkey[0], Decimal("49.996"))
        assert txid1 in self.nodes[0].getrawmempool()

        tx1_hex = self.nodes[0].gettransaction(txid1)['hex']
        tx1 = tx_from_hex(tx1_hex)

        # Check that wtxid is properly reported in mempool entry (txid1)
        assert_equal(self.nodes[0].getmempoolentry(txid1)["wtxid"], tx1.wtxid_hex)

        # Check that weight and vsize are properly reported in mempool entry (txid1)
        assert_equal(self.nodes[0].getmempoolentry(txid1)["vsize"], tx1.get_vsize())
        assert_equal(self.nodes[0].getmempoolentry(txid1)["weight"], tx1.get_weight())

        # Now create tx2, which will spend from txid1.
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(int(txid1, 16), 0), b''))
        tx.vout.append(CTxOut(int(49.99 * COIN), CScript([OP_TRUE, OP_DROP] * 15 + [OP_TRUE])))
        tx.lock_height = tx1.lock_height
        tx2_hex = self.nodes[0].signrawtransactionwithwallet(tx.serialize().hex())['hex']
        txid2 = self.nodes[0].sendrawtransaction(tx2_hex)
        tx = tx_from_hex(tx2_hex)
        assert not tx.wit.is_null()

        # Check that wtxid is properly reported in mempool entry (txid2)
        assert_equal(self.nodes[0].getmempoolentry(txid2)["wtxid"], tx.wtxid_hex)

        # Check that weight and vsize are properly reported in mempool entry (txid2)
        assert_equal(self.nodes[0].getmempoolentry(txid2)["vsize"], tx.get_vsize())
        assert_equal(self.nodes[0].getmempoolentry(txid2)["weight"], tx.get_weight())

        # Now create tx3, which will spend from txid2
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(int(txid2, 16), 0), b""))
        tx.vout.append(CTxOut(int(49.95 * COIN), CScript([OP_TRUE, OP_DROP] * 15 + [OP_TRUE])))  # Huge fee
        tx.lock_height = tx1.lock_height
        tx.calc_sha256()
        txid3 = self.nodes[0].sendrawtransaction(hexstring=tx.serialize().hex(), maxfeerate=0)
        assert tx.wit.is_null()
        assert txid3 in self.nodes[0].getrawmempool()

        # Check that getblocktemplate includes all transactions.
        template = self.nodes[0].getblocktemplate({"rules": ["segwit","finaltx","auxpow"]})
        template_txids = [t['txid'] for t in template['transactions']]
        assert txid1 in template_txids
        assert txid2 in template_txids
        assert txid3 in template_txids

        # Check that wtxid is properly reported in mempool entry (txid3)
        assert_equal(self.nodes[0].getmempoolentry(txid3)["wtxid"], tx.wtxid_hex)

        # Check that weight and vsize are properly reported in mempool entry (txid3)
        assert_equal(self.nodes[0].getmempoolentry(txid3)["vsize"], tx.get_vsize())
        assert_equal(self.nodes[0].getmempoolentry(txid3)["weight"], tx.get_weight())

        # Mine a block to clear the gbt cache again.
        self.generate(self.nodes[0], 1)

    def mine_and_test_listunspent(self, script_list, ismine):
        utxo = find_spendable_utxo(self.nodes[0], 50)
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(int('0x' + utxo['txid'], 0), utxo['vout'])))
        tx.lock_height = utxo['refheight']
        for i in script_list:
            tx.vout.append(CTxOut(10000000, i))
        signresults = self.nodes[0].signrawtransactionwithwallet(tx.serialize_without_witness().hex())['hex']
        txid = self.nodes[0].sendrawtransaction(hexstring=signresults, maxfeerate=0)
        txs_mined[txid] = self.generate(self.nodes[0], 1)[0]
        watchcount = 0
        spendcount = 0
        for i in self.nodes[0].listunspent():
            if i['txid'] == txid:
                watchcount += 1
                if i['spendable']:
                    spendcount += 1
        if ismine == 2:
            assert_equal(spendcount, len(script_list))
        elif ismine == 1:
            assert_equal(watchcount, len(script_list))
            assert_equal(spendcount, 0)
        else:
            assert_equal(watchcount, 0)
        return txid

    def p2sh_address_to_script(self, v):
        p2pk = CScript(bytes.fromhex(v['hex']))
        p2sh = CScript(bytes.fromhex(v['scriptPubKey']))
        p2wsh_long = script_to_p2wsh_script(p2pk)
        p2sh_p2wsh_long = script_to_p2sh_script(p2wsh_long)
        p2wsh_short = script_to_p2wpk_script(p2pk)
        p2sh_p2wsh_short = script_to_p2sh_script(p2wsh_short)
        _skip = hash256(bytes([0]) + CScript([OP_TRUE]))
        proof = bytes([1]) + (bytes([1]) + _skip)
        _mast = fastHash256(_skip, hash256(bytes([0]) + p2pk))
        p2mast_long = CScript([OP_0, _mast])
        p2mast_short = CScript([OP_0, ripemd160(_mast)])
        return([p2pk, p2sh, p2wsh_long, p2sh_p2wsh_long, p2wsh_short, p2sh_p2wsh_short, p2mast_long, p2mast_short, proof])

    def p2pkh_address_to_script(self, v):
        pubkey = bytes.fromhex(v['pubkey'])
        p2pk = key_to_p2pk_script(pubkey)
        p2sh = script_to_p2sh_script(p2pk)
        p2wsh_long = script_to_p2wsh_script(p2pk)
        p2sh_p2wsh_long = script_to_p2sh_script(p2wsh_long)
        p2wsh_short = key_to_p2wpk_script(pubkey)
        p2sh_p2wsh_short = script_to_p2sh_script(p2wsh_short)
        _skip = hash256(bytes([0]) + CScript([OP_TRUE]))
        proof = bytes([1]) + (bytes([1]) + _skip)
        _mast = fastHash256(_skip, hash256(bytes([0]) + p2pk))
        p2mast_long = CScript([OP_0, _mast])
        p2mast_short = CScript([OP_0, ripemd160(_mast)])
        p2pkh = CScript(bytes.fromhex(v['scriptPubKey']))
        p2sh_p2pkh = script_to_p2sh_script(p2pkh)
        p2wsh_long_p2pkh = script_to_p2wsh_script(p2pkh)
        p2sh_p2wsh_long_p2pkh = script_to_p2sh_script(p2wsh_long_p2pkh)
        p2wsh_short_p2pkh = script_to_p2wpk_script(p2pkh)
        p2sh_p2wsh_short_p2pkh = script_to_p2sh_script(p2wsh_short_p2pkh)
        _mast = fastHash256(_skip, hash256(bytes([0]) + p2pkh))
        p2mast_long_p2pkh = CScript([OP_0, _mast])
        p2mast_short_p2pkh = CScript([OP_0, ripemd160(_mast)])
        return [p2pk, p2sh, p2wsh_long, p2sh_p2wsh_long, p2wsh_short, p2sh_p2wsh_short, p2mast_long, p2mast_short, p2pkh, p2sh_p2pkh, p2wsh_long_p2pkh, p2sh_p2wsh_long_p2pkh, p2wsh_short_p2pkh, p2sh_p2wsh_short_p2pkh, p2mast_long_p2pkh, p2mast_short_p2pkh, proof]

    def create_and_mine_tx_from_txids(self, txids, success=True):
        tx = CTransaction()
        for i in txids:
            txraw = self.nodes[0].getrawtransaction(i, 0, txs_mined[i])
            txtmp = tx_from_hex(txraw)
            for j in range(len(txtmp.vout)):
                tx.vin.append(CTxIn(COutPoint(int('0x' + i, 0), j)))
                tx.lock_height = max(tx.lock_height, txtmp.lock_height)
        tx.vout.append(CTxOut(0, CScript()))
        signresults = self.nodes[0].signrawtransactionwithwallet(tx.serialize_without_witness().hex())['hex']
        self.nodes[0].sendrawtransaction(hexstring=signresults, maxfeerate=0)
        self.generate(self.nodes[0], 1)


if __name__ == '__main__':
    SegWitTest(__file__).main()
