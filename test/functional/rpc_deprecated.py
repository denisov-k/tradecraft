#!/usr/bin/env python3
# Copyright (c) 2017-2021 The Bitcoin Core developers
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
"""Test deprecation of RPC calls."""
from test_framework.test_framework import FreicoinTestFramework

class DeprecatedRpcTest(FreicoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[]]
        self.uses_wallet = None

    def run_test(self):
        # This test should be used to verify the errors of the currently
        # deprecated RPC methods (without the -deprecatedrpc flag) until
        # such RPCs are fully removed. For example:
        #
        # self.log.info("Test generate RPC")
        # assert_raises_rpc_error(-32, 'The wallet generate rpc method is deprecated', self.nodes[0].generate, 1)
        #
        # Please ensure that for all the RPC methods tested here, there is
        # at least one other functional test that still tests the RPCs
        # functionality using the respective -deprecatedrpc flag.

        # Please don't delete nor modify this comment
        self.log.info("Tests for deprecated RPC methods (if any)")

        if self.is_wallet_compiled():
            self.log.info("Tests for deprecated wallet-related RPC methods (if any)")
            self.log.info("Test settxfee RPC deprecation")
            self.nodes[0].createwallet("settxfeerpc")
            assert_raises_rpc_error(-32, 'settxfee is deprecated and will be fully removed in v31.0.', self.nodes[0].settxfee, 0.01)

if __name__ == '__main__':
    DeprecatedRpcTest(__file__).main()
