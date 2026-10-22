#!/usr/bin/env python3
"""Decode a Freicoin block header hex (from `getblockheader <hash> false`) and print the aux-pow
(merge-mining) fields: parent block version/prev/nBits/nonce, number of txs in the parent block,
midstate length. Standalone, no dependencies. Usage: verify_aux.py <header-hex-file-or-hex>"""
import sys, struct

def cs(f):
    b = f.read(1)[0]
    if b < 253: return b
    if b == 253: return struct.unpack("<H", f.read(2))[0]
    if b == 254: return struct.unpack("<I", f.read(4))[0]
    return struct.unpack("<Q", f.read(8))[0]

def varint(f):  # bitcoin VARINT (MSB-base128)
    n = 0
    while True:
        b = f.read(1)[0]; n = (n << 7) | (b & 0x7f)
        if b & 0x80: n += 1
        else: return n

def u256(f): return f.read(32)[::-1].hex()

def main():
    arg = sys.argv[1]
    try: hx = open(arg).read().strip()
    except OSError: hx = arg.strip()
    from io import BytesIO
    f = BytesIO(bytes.fromhex(hx))
    ver, = struct.unpack("<i", f.read(4)); prev = u256(f); mr = u256(f); t, bits, nonce = struct.unpack("<III", f.read(12))
    ext = bool(bits & 0x00800000); bits &= ~0x00800000
    print("nVersion %08x  nTime %d  nBits %08x  nNonce %08x  extended=%s" % (ver & 0xffffffff, t, bits, nonce, ext))
    if not ext: print("no aux-pow in this header"); return
    dummy = f.read(1)[0]; flags = f.read(1)[0]; assert dummy == 0xff and flags & 1
    cv, = struct.unpack("<i", f.read(4)); cmr = u256(f); ct, cb, cn = struct.unpack("<III", f.read(12)); slo, shi = struct.unpack("<QQ", f.read(16))
    ncb = cs(f); [ (f.read(1), f.read(32)) for _ in range(ncb) ]
    mh = u256(f); mbl = cs(f); mb = f.read(mbl); ml = varint(f); lt, = struct.unpack("<I", f.read(4))
    nb = cs(f); branch = [u256(f) for _ in range(nb)]; ntx = varint(f) + 1
    av, = struct.unpack("<i", f.read(4)); aprev = u256(f); abits, anonce = struct.unpack("<II", f.read(8))
    print("aux commit: bits %08x  bias %d  secret_zero=%s  commit_branch=%d  midstate_len=%d (buffer %d B)" % (cb, cn & 0xff, slo == 0 and shi == 0, ncb, ml, mbl))
    print("PARENT block: version %08x  prev %s  nBits %08x  nonce %08x  txs_in_parent=%d  branch=%d" % (av & 0xffffffff, aprev, abits, anonce, ntx, nb))
    print("verdict:", "merge-mined with a real parent block (%d txs)" % ntx if ntx > 1 or ml > 0 else "fake parent (built-in stratum style)")

if __name__ == "__main__": main()
