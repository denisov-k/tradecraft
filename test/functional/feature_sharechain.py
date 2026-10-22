#!/usr/bin/env python3
# Copyright (c) 2026 The Freicoin Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Share chain end to end: shares mined through the stratum server on two nodes are relayed,
both nodes converge on one share chain, coinbases pay every contributor, a late node syncs the
whole chain with getsharechain, and the chain survives a restart (sharechain.dat)."""

import hashlib
import json
import socket
import struct
import time

from test_framework.test_framework import FreicoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than_or_equal,
    stratum_port,
)

# regtest share target: compact 0x207fffff
SHARE_TARGET = 0x7fffff << (8 * (0x20 - 3))
SHARE_COMMIT_ID = "4b4a4948"


def dsha(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def swap32(h):
    b = bytes.fromhex(h)
    return b"".join(b[i:i + 4][::-1] for i in range(0, len(b), 4))


class StratumMiner:
    """Just enough stratum v1 to take one work unit, find a hash under the share target and
    submit it. Message dispatch looks at `method` first: the server reuses small ids."""

    def __init__(self, port, address, log):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=60)
        self.f = self.sock.makefile("rw")
        self.log = log
        self.address = address
        self.extranonce1 = None
        self.en2size = 4
        self.send({"id": 1, "method": "mining.subscribe", "params": ["feature_sharechain/0.1"]})
        self.send({"id": 2, "method": "mining.authorize", "params": [address + "+0.000001", "x"]})

    def send(self, m):
        self.f.write(json.dumps(m) + "\n")
        self.f.flush()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def mine_one(self):
        """Wait for work, solve it, submit, and return the submit result."""
        while True:
            line = self.f.readline()
            if not line:
                raise AssertionError("stratum connection closed")
            m = json.loads(line)
            if m.get("method") == "mining.notify":
                if self.extranonce1 is None:
                    continue
                # work may have been superseded while we were idle (new block or share chain
                # tip): drain queued notifications and mine the newest
                job = m["params"]
                self.sock.settimeout(0.5)
                try:
                    while True:
                        extra = self.f.readline()
                        if not extra:
                            break
                        em = json.loads(extra)
                        if em.get("method") == "mining.notify":
                            job = em["params"]
                except (socket.timeout, TimeoutError):
                    pass
                finally:
                    self.sock.settimeout(60)
                return self.solve_and_submit(job)
            if m.get("method") == "mining.set_difficulty":
                continue
            if m.get("id") == 1 and m.get("result"):
                self.extranonce1 = m["result"][1]
                self.en2size = m["result"][2]

    def solve_and_submit(self, job):
        job_id, prevhash, coinb1, coinb2, merkle, version, nbits, ntime = job[:8]
        en2hex = "00" * self.en2size
        coinbase = bytes.fromhex(coinb1 + self.extranonce1 + en2hex + coinb2)
        root = dsha(coinbase)
        for br in merkle:
            root = dsha(root + bytes.fromhex(br))
        hdr = struct.pack("<I", int(version, 16)) + swap32(prevhash) + root + struct.pack("<I", int(ntime, 16)) + struct.pack("<I", int(nbits, 16))
        for nonce in range(0, 1 << 20):
            h = dsha(hdr + struct.pack("<I", nonce))
            if int.from_bytes(h, "little") <= SHARE_TARGET:
                self.send({"id": 100, "method": "mining.submit", "params": [self.address + "+0.000001", job_id, en2hex, ntime, "%08x" % nonce]})
                # the reply may be preceded by new work; keep reading until the id-100 reply
                while True:
                    line = self.f.readline()
                    if not line:
                        raise AssertionError("stratum connection closed after submit")
                    r = json.loads(line)
                    if r.get("id") == 100:
                        return r.get("result"), r.get("error")
        raise AssertionError("no share found in 2^20 nonces")


class ShareChainTest(FreicoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [self.node_args(i) for i in range(3)]

    def node_args(self, i):
        return [
            "-stratum=1",
            "-stratumbind=127.0.0.1",
            "-stratumport=%d" % stratum_port(i),
            "-stratumallowip=127.0.0.1",
            "-sharechain=regtest",
            "-miningmindiff=0.000001",
            "-debug=stratum",
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        # node2 stays isolated until the sync test
        self.connect_nodes(0, 1)

    def sc(self, node):
        return node.getsharechaininfo()

    def state(self, node):
        i = self.sc(node)
        return (i["height"], i["tip"], i["chainshares"], i["fruits"])

    def wait_same_tip(self, a, b, timeout=60):
        """Both nodes know the same shares and agree on the chain tip (which may still be empty
        while only fruits exist: one share in chainfactor is a chain share)."""
        self.wait_until(lambda: self.state(a) == self.state(b), timeout=timeout)

    def run_test(self):
        n0, n1, n2 = self.nodes
        self.log.info("bootstrap: 110 blocks on node0, node1 follows")
        self.generate(n0, 110, sync_fun=lambda: self.sync_blocks([n0, n1]))
        addr = [n.getnewaddress("", "bech32") for n in self.nodes]

        info = self.sc(n0)
        assert_equal(info["enabled"], True)
        assert_equal(info["network"], "regtest")
        assert_equal(info["sharebits"], "207fffff")
        assert_equal(info["height"], -1)

        self.log.info("mine shares alternately on node0 and node1; each must reach the other")
        miners = [StratumMiner(stratum_port(i), addr[i], self.log) for i in (0, 1)]
        rounds = 6
        for r in range(rounds):
            for i in (0, 1):
                result, error = miners[i].mine_one()
                assert_equal(error, None)
                self.wait_until(lambda: self.sc(self.nodes[i])["fruits"] >= 2 * r + i + 1, timeout=30)
                self.wait_same_tip(n0, n1)
        for m in miners:
            m.close()

        a, b = self.sc(n0), self.sc(n1)
        assert_equal(self.state(n0), self.state(n1))
        assert_greater_than_or_equal(a["fruits"], 2 * rounds)
        assert_greater_than_or_equal(a["chainshares"], 1)
        assert a["tip"] != ""
        self.log.info("share chain on both nodes: height %d, %d chain shares, %d fruits", a["height"], a["chainshares"], a["fruits"])

        self.log.info("every regtest share is also a block: the tip coinbase pays both miners and ends with the share commitment")
        self.sync_blocks([n0, n1])
        tip = n0.getblock(n0.getbestblockhash(), 2)
        vout = tip["tx"][0]["vout"]
        payees = {o["scriptPubKey"].get("address") for o in vout if o["scriptPubKey"].get("address")}
        assert addr[0] in payees and addr[1] in payees, payees
        commit = vout[-1]["scriptPubKey"]["hex"]
        assert commit.startswith("6a24") and commit.endswith(SHARE_COMMIT_ID), commit
        assert_equal(vout[-1]["value"], 0)

        self.log.info("node2 joins late and syncs the whole share chain with getsharechain")
        self.connect_nodes(2, 0)
        self.sync_blocks()
        self.wait_same_tip(n0, n2, timeout=90)
        c = self.sc(n2)
        assert_equal(c["chainshares"], a["chainshares"])
        assert_equal(c["fruits"], a["fruits"])

        self.log.info("node0 restarts and reloads its share chain from sharechain.dat")
        before = self.sc(n0)
        self.restart_node(0, extra_args=self.node_args(0))
        after = self.sc(n0)
        assert_equal(after["tip"], before["tip"])
        assert_equal(after["chainshares"], before["chainshares"])
        assert_equal(after["fruits"], before["fruits"])
        self.connect_nodes(0, 1)

        self.log.info("a share mined after the restart still propagates")
        m = StratumMiner(stratum_port(0), addr[0], self.log)
        result, error = m.mine_one()
        m.close()
        assert_equal(error, None)
        self.wait_until(lambda: self.sc(n0)["fruits"] > before["fruits"], timeout=30)
        self.wait_same_tip(n0, n1)


if __name__ == "__main__":
    ShareChainTest(__file__).main()
