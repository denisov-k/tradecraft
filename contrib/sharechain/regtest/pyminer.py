#!/usr/bin/env python3
"""Minimal stratum v1 SHA256d miner for regtest (no dependencies). Not fast; use with a low
share difficulty (bitcoind -miningmindiff=0.001). Usage: pyminer.py host port username [password]"""
import socket, json, hashlib, struct, sys, time, binascii

host, port, user = sys.argv[1], int(sys.argv[2]), sys.argv[3]
password = sys.argv[4] if len(sys.argv) > 4 else "x"
# optional 5th arg: local share difficulty override. Mark's stratum servers do not enforce the share
# target on submit (they only check the real block/aux targets), so a CPU miner can pick its own.
force_diff = float(sys.argv[5]) if len(sys.argv) > 5 else None

def dsha(b): return hashlib.sha256(hashlib.sha256(b).digest()).digest()
def swap32(h): b = bytes.fromhex(h); return b"".join(b[i:i+4][::-1] for i in range(0, len(b), 4))

s = socket.create_connection((host, port)); f = s.makefile("rw")
def send(m): f.write(json.dumps(m) + "\n"); f.flush()
send({"id": 1, "method": "mining.subscribe", "params": ["pyminer/0.1"]})
send({"id": 2, "method": "mining.authorize", "params": [user, password]})
extranonce1 = None; en2size = 4; difficulty = force_diff or 1.0; job = None; shares = 0; accepted = 0

def target_from_diff(d):
    return int(0x00000000ffff0000000000000000000000000000000000000000000000000000 / d)

while True:
    line = f.readline()
    if not line: print("disconnected"); break
    m = json.loads(line)
    if m.get("method") == "mining.set_difficulty":
        srv = float(m["params"][0]); difficulty = force_diff or srv; print("server difficulty", srv, "-> using", difficulty, flush=True)
    elif m.get("id") == 1 and m.get("result"):
        extranonce1 = m["result"][1]; en2size = m["result"][2]; print("subscribed extranonce1=%s en2size=%d" % (extranonce1, en2size), flush=True)
    elif m.get("id") == 2: print("authorized:", m.get("result"), m.get("error"), flush=True)
    elif m.get("id", 0) >= 100:
        shares += 1; accepted += 1 if m.get("result") else 0
        print("share result:", m.get("result"), m.get("error"), "(%d/%d accepted)" % (accepted, shares), flush=True)
    elif m.get("method") == "mining.notify":
        job = m["params"]; job_id, prevhash, coinb1, coinb2, merkle, version, nbits, ntime, clean = job[:9]
        if extranonce1 is None: continue
        target = target_from_diff(difficulty)
        en2 = 0; found = False; t0 = time.time(); hashes = 0
        while not found and en2 < 2**20:
            en2hex = ("%0" + str(en2size*2) + "x") % en2
            coinbase = bytes.fromhex(coinb1 + extranonce1 + en2hex + coinb2)
            root = dsha(coinbase)
            for br in merkle: root = dsha(root + bytes.fromhex(br))
            header_pre = bytes.fromhex(version)[::-1] + swap32(prevhash)[::-1] if False else None
            # build header: version(LE) prevhash(as given, word-swapped) merkleroot(LE) ntime(LE) nbits(LE) nonce(LE)
            hdr = struct.pack("<I", int(version, 16)) + swap32(prevhash) + root + struct.pack("<I", int(ntime, 16)) + struct.pack("<I", int(nbits, 16))
            for nonce in range(0, 2**32, 1):
                h = dsha(hdr + struct.pack("<I", nonce)); hashes += 1
                if int.from_bytes(h, "little") <= target:
                    send({"id": 100 + shares, "method": "mining.submit", "params": [user, job_id, en2hex, ntime, "%08x" % nonce]})
                    print("submitted share job=%s en2=%s nonce=%08x after %d hashes (%.0f H/s)" % (job_id, en2hex, nonce, hashes, hashes / max(time.time()-t0, 1e-6)), flush=True)
                    found = True; break
                if hashes % 200000 == 0:
                    # check for new work without blocking
                    s.setblocking(False)
                    try:
                        peek = s.recv(1, socket.MSG_PEEK)
                    except BlockingIOError: peek = b""
                    s.setblocking(True)
                    if peek: found = True; break   # new message pending: go handle it
            en2 += 1
