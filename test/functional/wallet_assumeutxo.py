#!/usr/bin/env python3
# Copyright (c) 2023-present The Bitcoin Core developers
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
"""Test for assumeutxo wallet related behavior.
See feature_assumeutxo.py for background.

## Possible test improvements

- TODO: test loading a wallet (backup) on a pruned node

"""
from decimal import Decimal

from test_framework.address import address_to_scriptpubkey
from test_framework.descriptors import descsum_create
from test_framework.test_framework import FreicoinTestFramework
from test_framework.messages import COIN
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    ensure_for,
)
from test_framework.wallet import MiniWallet
from test_framework.wallet_util import get_generate_key

START_HEIGHT = 199
SNAPSHOT_BASE_HEIGHT = 299
FINAL_HEIGHT = 399


class AssumeutxoTest(FreicoinTestFramework):
    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        """Use the pregenerated, deterministic chain up to height 199."""
        self.num_nodes = 3
        self.rpc_timeout = 120
        self.extra_args = [
            [],
            [],
            [],
        ]

    def setup_network(self):
        """Start with the nodes disconnected so that one can generate a snapshot
        including blocks the other hasn't yet seen."""
        self.add_nodes(3)
        self.start_nodes(extra_args=self.extra_args)

    def import_descriptor(self, node, wallet_name, key, timestamp):
        import_request = [{"desc": descsum_create("pkh(" + key.pubkey + ")"),
                           "timestamp": timestamp,
                           "label": "Descriptor import test"}]
        wrpc = node.get_wallet_rpc(wallet_name)
        return wrpc.importdescriptors(import_request)

    def run_test(self):
        """
        Bring up two (disconnected) nodes, mine some new blocks on the first,
        and generate a UTXO snapshot.

        Load the snapshot into the second, ensure it syncs to tip and completes
        background validation when connected to the first.
        """
        n0 = self.nodes[0]
        n1 = self.nodes[1]
        n2 = self.nodes[2]

        self.mini_wallet = MiniWallet(n0)

        # Mock time for a deterministic chain
        for n in self.nodes:
            n.setmocktime(n.getblockheader(n.getbestblockhash())['time'])

        # Create a wallet that we will create a backup for later (at snapshot height)
        n0.createwallet('w')
        w = n0.get_wallet_rpc("w")
        w_address = w.getnewaddress()

        # Create another wallet and backup now (before snapshot height)
        n0.createwallet('w2')
        w2 = n0.get_wallet_rpc("w2")
        w2_address = w2.getnewaddress()
        w2.backupwallet("backup_w2.dat")

        # Generate a series of blocks that `n0` will have in the snapshot,
        # but that n1 doesn't yet see. In order for the snapshot to activate,
        # though, we have to ferry over the new headers to n1 so that it
        # isn't waiting forever to see the header of the snapshot's base block
        # while disconnected from n0.
        for i in range(100):
            if i % 3 == 0:
                self.mini_wallet.send_self_transfer(from_node=n0)
            self.generate(n0, nblocks=1, sync_fun=self.no_op)
            newblock = n0.getblock(n0.getbestblockhash(), 0)

            # make n1 aware of the new header, but don't give it the block.
            n1.submitheader(newblock)
            n2.submitheader(newblock)

        # Ensure everyone is seeing the same headers.
        for n in self.nodes:
            assert_equal(n.getblockchaininfo()[
                         "headers"], SNAPSHOT_BASE_HEIGHT)

        # This backup is created at the snapshot height, so it's
        # not part of the background sync anymore
        w.backupwallet("backup_w.dat")

        self.log.info("-- Testing assumeutxo")

        assert_equal(n0.getblockcount(), SNAPSHOT_BASE_HEIGHT)
        assert_equal(n1.getblockcount(), START_HEIGHT)

        self.log.info(
            f"Creating a UTXO snapshot at height {SNAPSHOT_BASE_HEIGHT}")
        dump_output = n0.dumptxoutset('utxos.dat', "latest")

        assert_equal(
            dump_output['txoutset_hash'],
            "0979d10b7040b5809a49a8ef89065325adbcc6335b65d108426d5b86c2df471e")
        assert_equal(dump_output["nchaintx"], 533)
        assert_equal(n0.getblockchaininfo()["blocks"], SNAPSHOT_BASE_HEIGHT)

        # Mine more blocks on top of the snapshot that n1 hasn't yet seen. This
        # will allow us to test n1's sync-to-tip on top of a snapshot.
        w_skp = address_to_scriptpubkey(w_address)
        w2_skp = address_to_scriptpubkey(w2_address)
        for i in range(100):
            if i % 3 == 0:
                self.mini_wallet.send_to(from_node=n0, scriptPubKey=w_skp, amount=1 * COIN)
                self.mini_wallet.send_to(from_node=n0, scriptPubKey=w2_skp, amount=10 * COIN)
            self.generate(n0, nblocks=1, sync_fun=self.no_op)

        assert_equal(n0.getblockcount(), FINAL_HEIGHT)
        assert_equal(n1.getblockcount(), START_HEIGHT)
        assert_equal(n2.getblockcount(), START_HEIGHT)

        assert_equal(n0.getblockchaininfo()["blocks"], FINAL_HEIGHT)

        self.log.info(
            f"Loading snapshot into second node from {dump_output['path']}")
        loaded = n1.loadtxoutset(dump_output['path'])
        assert_equal(loaded['coins_loaded'], SNAPSHOT_BASE_HEIGHT + 1)
        assert_equal(loaded['base_height'], SNAPSHOT_BASE_HEIGHT)

        normal, snapshot = n1.getchainstates()["chainstates"]
        assert_equal(normal['blocks'], START_HEIGHT)
        assert_equal(normal.get('snapshot_blockhash'), None)
        assert_equal(normal['validated'], True)
        assert_equal(snapshot['blocks'], SNAPSHOT_BASE_HEIGHT)
        assert_equal(snapshot['snapshot_blockhash'], dump_output['base_hash'])
        assert_equal(snapshot['validated'], False)

        assert_equal(n1.getblockchaininfo()["blocks"], SNAPSHOT_BASE_HEIGHT)

        self.log.info("Backup from the snapshot height can be loaded during background sync")
        n1.restorewallet("w", "backup_w.dat")
        # Balance of w wallet is still still 0 because n1 has not synced yet
        assert_equal(n1.getbalance(), 0)

        self.log.info("Backup from before the snapshot height can't be loaded during background sync")
        assert_raises_rpc_error(-4, "Wallet loading failed. Error loading wallet. Wallet requires blocks to be downloaded, and software does not currently support loading wallets while blocks are being downloaded out of order when using assumeutxo snapshots. Wallet should be able to load successfully after node sync reaches height 299", n1.restorewallet, "w2", "backup_w2.dat")

        self.log.info("Test loading descriptors during background sync")
        wallet_name = "w1"
        n1.createwallet(wallet_name, disable_private_keys=True)
        key = get_generate_key()
        time = n1.getblockchaininfo()['time']
        timestamp = 0
        expected_error_message = f"Rescan failed for descriptor with timestamp {timestamp}. There was an error reading a block from time {time}, which is after or within 7200 seconds of key creation, and could contain transactions pertaining to the desc. As a result, transactions and coins using this desc may not appear in the wallet. This error is likely caused by an in-progress assumeutxo background sync. Check logs or getchainstates RPC for assumeutxo background sync progress and try again later."
        result = self.import_descriptor(n1, wallet_name, key, timestamp)
        assert_equal(result[0]['error']['code'], -1)
        assert_equal(result[0]['error']['message'], expected_error_message)

        self.log.info("Test that rescanning blocks from before the snapshot fails when blocks are not available from the background sync yet")
        w1 = n1.get_wallet_rpc(wallet_name)
        assert_raises_rpc_error(-1, "Failed to rescan unavailable blocks likely due to an in-progress assumeutxo background sync. Check logs or getchainstates RPC for assumeutxo background sync progress and try again later.", w1.rescanblockchain, 100)

        PAUSE_HEIGHT = FINAL_HEIGHT - 40

        self.log.info("Restarting node to stop at height %d", PAUSE_HEIGHT)
        self.restart_node(1, extra_args=[
            f"-stopatheight={PAUSE_HEIGHT}", *self.extra_args[1]])

        # Finally connect the nodes and let them sync.
        #
        # Set `wait_for_connect=False` to avoid a race between performing connection
        # assertions and the -stopatheight tripping.
        self.connect_nodes(0, 1, wait_for_connect=False)

        n1.wait_until_stopped(timeout=5)

        self.log.info(
            "Restarted node before snapshot validation completed, reloading...")
        self.restart_node(1, extra_args=self.extra_args[1])

        # TODO: inspect state of e.g. the wallet before reconnecting
        self.connect_nodes(0, 1)

        self.log.info(
            f"Ensuring snapshot chain syncs to tip. ({FINAL_HEIGHT})")
        self.wait_until(lambda: n1.getchainstates()[
                        'chainstates'][-1]['blocks'] == FINAL_HEIGHT)
        self.sync_blocks(nodes=(n0, n1))

        self.log.info("Ensuring background validation completes")
        self.wait_until(lambda: len(n1.getchainstates()['chainstates']) == 1)

        self.log.info("Ensuring wallet can be restored from a backup that was created before the snapshot height")
        n1.restorewallet("w2", "backup_w2.dat")
        # Check balance of w2 wallet
        # Freicoin: balances are time-value adjusted (demurrage), so the
        # present value of the 340 FRC nominal is slightly lower at this height.
        assert_equal(n1.getbalance(), Decimal("339.89864886"))

        # Check balance of w wallet after node is synced
        n1.loadwallet("w")
        w = n1.get_wallet_rpc("w")
        # Freicoin: demurrage-adjusted present value of the 34 FRC nominal.
        assert_equal(w.getbalance(), Decimal("33.98989430"))

        self.log.info("Check balance of a wallet that is active during snapshot completion")
        n2.restorewallet("w", "backup_w.dat")
        loaded = n2.loadtxoutset(dump_output['path'])
        self.connect_nodes(0, 2)
        self.wait_until(lambda: len(n2.getchainstates()['chainstates']) == 1)
        # Freicoin: demurrage-adjusted present value of the 34 FRC nominal.
        ensure_for(duration=1, f=lambda: (n2.getbalance() == Decimal("33.98989430")))

        self.log.info("Ensuring descriptors can be loaded after background sync")
        n1.loadwallet(wallet_name)
        result = self.import_descriptor(n1, wallet_name, key, timestamp)
        assert_equal(result[0]['success'], True)


if __name__ == '__main__':
    AssumeutxoTest(__file__).main()
