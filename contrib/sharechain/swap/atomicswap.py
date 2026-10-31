#!/usr/bin/env python3
"""Atomic swap of BTC for FRC: an HTLC on each chain under one hash, no third party.

A has BTC and wants FRC; B has FRC and wants BTC.  The side that locks longer must be the one
that learns the secret last, so A (who picks the secret) locks BTC for long and B locks FRC
for short:

  1. A: initiate     pick P, lock BTC: B's key with P, or A's key after bitcoin height T_btc.
                     Writes A's state file and prints the offer for B.
  2. B: participate  check the BTC output (script, amount, confirmations, T_btc far enough),
                     lock FRC: A's key with P, or B's key after Freicoin height T_frc.
  3. A: redeem-frc   spend the FRC HTLC with P; P is now on the Freicoin chain.
  4. B: redeem-btc   find P in A's Freicoin spend, spend the BTC HTLC with it.
  5. refund-btc / refund-frc after the timeouts, if the other side walked away.

B's safety needs T_frc to come well before T_btc in wall-clock time: B picks T_frc in Freicoin
blocks and demands T_btc in bitcoin blocks with a wide margin (--min-btc-blocks); Freicoin
block times vary, so the margin should cover a stall of the Freicoin chain.

The Freicoin side reuses frchtlc.py.  The bitcoin side is written here: P2WSH (single SHA256
of the script), BIP143 signatures, bitcoin transaction serialization (no lock_height).
"""
import argparse
import hashlib
import json
import os
import secrets
import sys
from decimal import Decimal

import frchtlc
from common import Rpc
from frchtlc import (CScript, ECKey, OP_CHECKLOCKTIMEVERIFY, OP_CHECKSIG, OP_DROP, OP_ELSE, OP_ENDIF,
                     OP_EQUALVERIFY, OP_IF, OP_SHA256, htlc_script, new_key, wif_to_key)
from test_framework.messages import ser_compact_size, ser_string  # noqa: E402
from test_framework.segwit_addr import decode_segwit_address, encode_segwit_address  # noqa: E402

SAT = 100_000_000
SIGHASH_ALL = 1
BTC_FEE = 2000  # sats; a redeem or refund is ~150 vbytes


# ---- bitcoin -----------------------------------------------------------------------------
def btc_hrp(rpc) -> str:
    return {"main": "bc", "test": "tb", "testnet4": "tb", "signet": "tb", "regtest": "bcrt"}[rpc("getblockchaininfo")["chain"]]


def btc_p2wsh_spk(script: bytes) -> bytes:
    return bytes([0x00, 0x20]) + hashlib.sha256(script).digest()


def btc_address_spk(address: str, hrp: str) -> bytes:
    ver, prog = decode_segwit_address(hrp, address)
    if ver is None:
        raise RuntimeError(f"{address} is not a {hrp} segwit address")
    return bytes([0x50 + ver if ver else 0x00, len(prog)]) + bytes(prog)


def _h256(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def btc_spend_htlc(rpc, script: bytes, txid: str, vout: int, key: ECKey, to_address: str,
                   preimage: bytes = None, timeout: int = None, fee: int = BTC_FEE) -> str:
    """Sign a spend of a bitcoin P2WSH HTLC output: redeem with `preimage`, or refund with
    nLockTime = `timeout`.  Returns the raw transaction hex."""
    utxo = rpc("gettxout", txid, vout, True)
    if not utxo:
        raise RuntimeError("the bitcoin HTLC output is spent or unknown")
    if utxo["scriptPubKey"]["hex"] != btc_p2wsh_spk(script).hex():
        raise RuntimeError("that output is not this HTLC")
    value = int(Decimal(str(utxo["value"])) * SAT)
    if value - fee <= 0:
        raise RuntimeError("the HTLC is worth less than the fee")
    refund = preimage is None
    version = (2).to_bytes(4, "little")
    outpoint = bytes.fromhex(txid)[::-1] + vout.to_bytes(4, "little")
    sequence = (0xfffffffe if refund else 0xffffffff).to_bytes(4, "little")
    out = (value - fee).to_bytes(8, "little") + ser_string(btc_address_spk(to_address, btc_hrp(rpc)))
    locktime = (timeout if refund else 0).to_bytes(4, "little")
    # BIP143
    msg = (version + _h256(outpoint) + _h256(sequence) + outpoint + ser_string(script)
           + value.to_bytes(8, "little") + sequence + _h256(out) + locktime + SIGHASH_ALL.to_bytes(4, "little"))
    sig = key.sign_ecdsa(_h256(msg)) + bytes([SIGHASH_ALL])
    stack = [sig, b"", script] if refund else [sig, preimage, b"\x01", script]
    witness = ser_compact_size(len(stack)) + b"".join(ser_string(i) for i in stack)
    tx = (version + b"\x00\x01" + ser_compact_size(1) + outpoint + ser_string(b"") + sequence
          + ser_compact_size(1) + out + witness + locktime)
    return tx.hex()


def btc_htlc_script(hashlock: bytes, redeemer_pub: bytes, refunder_pub: bytes, timeout: int) -> bytes:
    return bytes(CScript([OP_IF, OP_SHA256, hashlock, OP_EQUALVERIFY, redeemer_pub,
                          OP_ELSE, timeout, OP_CHECKLOCKTIMEVERIFY, OP_DROP, refunder_pub,
                          OP_ENDIF, OP_CHECKSIG]))


# ---- Freicoin: find the preimage A revealed ------------------------------------------------
def frc_find_preimage(frc, txid: str, vout: int, hashlock: bytes, from_height: int):
    """Look for the spend of the FRC HTLC (mempool, then blocks from `from_height`) and return
    the preimage in its witness."""
    def scan(tx):
        for i in tx.get("vin", []):
            if i.get("txid") == txid and i.get("vout") == vout:
                for item in i.get("txinwitness", []):
                    b = bytes.fromhex(item)
                    if len(b) == 32 and hashlib.sha256(b).digest() == hashlock:
                        return b
        return None
    for t in frc("getrawmempool"):
        try:
            p = scan(frc("getrawtransaction", t, True))
        except Exception:
            continue
        if p:
            return p
    for h in range(max(0, from_height), frc("getblockcount") + 1):
        for tx in frc("getblock", frc("getblockhash", h), 2)["tx"]:
            p = scan(tx)
            if p:
                return p
    return None


# ---- steps ---------------------------------------------------------------------------------
def initiate(btc, frc, b_btc_pub: bytes, btc_amount: Decimal, frc_amount: Decimal, timeout_blocks: int):
    preimage = secrets.token_bytes(32)
    hashlock = hashlib.sha256(preimage).digest()
    refund_key, frc_key = new_key(), new_key()
    t_btc = btc("getblockcount") + timeout_blocks
    script = btc_htlc_script(hashlock, b_btc_pub, refund_key.get_pubkey().get_bytes(), t_btc)
    address = encode_segwit_address(btc_hrp(btc), 0, hashlib.sha256(script).digest())
    txid = btc("sendtoaddress", address, str(btc_amount))
    spk = btc_p2wsh_spk(script).hex()
    vout = next(o["n"] for o in btc("gettransaction", txid, True, True)["decoded"]["vout"] if o["scriptPubKey"]["hex"] == spk)
    main = frchtlc.is_main(frc)
    offer = {"btctxid": txid, "btcvout": vout, "btc": str(btc_amount), "frc": str(frc_amount),
             "hashlock": hashlock.hex(), "btctimeout": t_btc, "btcscript": script.hex(),
             "afrcpubkey": frc_key.get_pubkey().get_bytes().hex(), "bbtcpubkey": b_btc_pub.hex()}
    state = dict(offer, preimage=preimage.hex(), btcrefundwif=frchtlc.key_to_wif(refund_key, main),
                 frcwif=frchtlc.key_to_wif(frc_key, main))
    return offer, state


def participate(btc, frc, offer, b_btc_key: ECKey, timeout_blocks: int, min_btc_blocks: int, min_conf: int):
    hashlock = bytes.fromhex(offer["hashlock"])
    script = bytes.fromhex(offer["btcscript"])
    if b_btc_key.get_pubkey().get_bytes().hex() != offer["bbtcpubkey"]:
        raise RuntimeError("the offer is for another bitcoin key")
    utxo = btc("gettxout", offer["btctxid"], int(offer["btcvout"]), True)
    if not utxo or utxo["scriptPubKey"]["hex"] != btc_p2wsh_spk(script).hex():
        raise RuntimeError("no such bitcoin HTLC output")
    # the script must pay us with the preimage of the offer's hashlock
    want_prefix = bytes(CScript([OP_IF, OP_SHA256, hashlock, OP_EQUALVERIFY, b_btc_key.get_pubkey().get_bytes(), OP_ELSE]))
    if not script.startswith(want_prefix) or script != btc_htlc_script(hashlock, b_btc_key.get_pubkey().get_bytes(), script[-35:-2], int(offer["btctimeout"])):
        raise RuntimeError("the bitcoin script is not an HTLC to our key under the offer's hashlock and timeout")
    if Decimal(str(utxo["value"])) < Decimal(offer["btc"]):
        raise RuntimeError("the bitcoin HTLC holds less than offered")
    if utxo["confirmations"] < min_conf:
        raise RuntimeError(f"{utxo['confirmations']} confirmations, we want {min_conf}")
    left = int(offer["btctimeout"]) - btc("getblockcount")
    if left < min_btc_blocks:
        raise RuntimeError(f"only {left} bitcoin blocks before A can refund; we want {min_btc_blocks}")
    refund_key = new_key()
    t_frc = frc("getblockcount") + timeout_blocks
    fscript = htlc_script(hashlock, bytes.fromhex(offer["afrcpubkey"]), refund_key.get_pubkey().get_bytes(), t_frc)
    faddress = frchtlc.htlc_address(fscript, frchtlc.is_main(frc))
    ftxid = frc("sendtoaddress", faddress, offer["frc"])
    spk = frchtlc.script_to_p2wsh_script(fscript).hex()
    fvout = next(o["n"] for o in frc("gettransaction", ftxid, True, True)["decoded"]["vout"] if o["scriptPubKey"]["hex"] == spk)
    reply = {"frctxid": ftxid, "frcvout": fvout, "frctimeout": t_frc, "frcscript": fscript.hex(),
             "frcheight": frc("getblockcount")}
    state = dict(offer, **reply, frcrefundwif=frchtlc.key_to_wif(refund_key, frchtlc.is_main(frc)))
    return reply, state


def check_participation(frc, state, reply, min_conf: int):
    """A's checks before revealing P: B's FRC HTLC pays A's key under the same hash."""
    a_key = wif_to_key(state["frcwif"])
    fscript = CScript(bytes.fromhex(reply["frcscript"]))
    want = htlc_script(bytes.fromhex(state["hashlock"]), a_key.get_pubkey().get_bytes(),
                       bytes(fscript)[-35:-2], int(reply["frctimeout"]))
    if bytes(want) != bytes(fscript):
        raise RuntimeError("B's Freicoin script is not an HTLC to our key under our hashlock")
    utxo = frc("gettxout", reply["frctxid"], int(reply["frcvout"]), True)
    if not utxo or utxo["scriptPubKey"]["hex"] != frchtlc.script_to_p2wsh_script(fscript).hex():
        raise RuntimeError("no such Freicoin HTLC output")
    if Decimal(str(utxo["amount"])) < Decimal(state["frc"]) * Decimal("0.999"):
        raise RuntimeError("the Freicoin HTLC holds less than agreed")
    if utxo["confirmations"] < min_conf:
        raise RuntimeError(f"{utxo['confirmations']} confirmations, we want {min_conf}")
    return fscript


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--btc-rpc", required=True, help="bitcoind RPC URL (with /wallet/<name> where a wallet pays)")
    ap.add_argument("--btc-rpcuser", required=True)
    ap.add_argument("--btc-rpcpassword", required=True)
    ap.add_argument("--frc-rpc", required=True, help="freicoind RPC URL (with /wallet/<name> where a wallet pays)")
    ap.add_argument("--frc-rpcuser", required=True)
    ap.add_argument("--frc-rpcpassword", required=True)
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("initiate", help="A: lock BTC")
    i.add_argument("--b-btc-pubkey", required=True)
    i.add_argument("--btc", required=True, type=Decimal)
    i.add_argument("--frc", required=True, type=Decimal)
    i.add_argument("--timeout-blocks", type=int, default=48, help="bitcoin blocks until A can refund")
    i.add_argument("--state", required=True)
    p = sub.add_parser("participate", help="B: check A's BTC lock, lock FRC")
    p.add_argument("--offer", required=True)
    p.add_argument("--btc-wif", required=True, help="B's bitcoin key (its public key went into the offer)")
    p.add_argument("--timeout-blocks", type=int, default=12, help="Freicoin blocks until B can refund")
    p.add_argument("--min-btc-blocks", type=int, default=36)
    p.add_argument("--min-conf", type=int, default=1)
    p.add_argument("--state", required=True)
    rf = sub.add_parser("redeem-frc", help="A: take the FRC (reveals the preimage)")
    rf.add_argument("--state", required=True)
    rf.add_argument("--reply", required=True)
    rf.add_argument("--to", required=True)
    rf.add_argument("--min-conf", type=int, default=1)
    rb = sub.add_parser("redeem-btc", help="B: take the BTC with the preimage from A's Freicoin spend")
    rb.add_argument("--state", required=True)
    rb.add_argument("--btc-wif", required=True)
    rb.add_argument("--to", required=True)
    for name, who in (("refund-btc", "A"), ("refund-frc", "B")):
        r = sub.add_parser(name, help=f"{who}: take the coins back after the timeout")
        r.add_argument("--state", required=True)
        r.add_argument("--to", required=True)
    a = ap.parse_args()
    btc = Rpc(a.btc_rpc, a.btc_rpcuser, a.btc_rpcpassword)
    frc = Rpc(a.frc_rpc, a.frc_rpcuser, a.frc_rpcpassword)

    def save(path, obj):
        with open(path, "w") as fh:
            json.dump(obj, fh, indent=1)
        os.chmod(path, 0o600)

    if a.cmd == "initiate":
        offer, state = initiate(btc, frc, bytes.fromhex(a.b_btc_pubkey), a.btc, a.frc, a.timeout_blocks)
        save(a.state, state)
        print(json.dumps(offer, indent=1))
    elif a.cmd == "participate":
        reply, state = participate(btc, frc, json.load(open(a.offer)), wif_to_key(a.btc_wif), a.timeout_blocks, a.min_btc_blocks, a.min_conf)
        save(a.state, state)
        print(json.dumps(reply, indent=1))
    elif a.cmd == "redeem-frc":
        state, reply = json.load(open(a.state)), json.load(open(a.reply))
        fscript = check_participation(frc, state, reply, a.min_conf)
        raw = frchtlc.spend_htlc(frc, fscript, reply["frctxid"], int(reply["frcvout"]), wif_to_key(state["frcwif"]), a.to,
                                 preimage=bytes.fromhex(state["preimage"]))
        print(json.dumps({"txid": frc("sendrawtransaction", raw)}))
    elif a.cmd == "redeem-btc":
        state = json.load(open(a.state))
        preimage = frc_find_preimage(frc, state["frctxid"], int(state["frcvout"]), bytes.fromhex(state["hashlock"]), int(state["frcheight"]))
        if not preimage:
            sys.exit("A has not spent the Freicoin HTLC yet: no preimage")
        raw = btc_spend_htlc(btc, bytes.fromhex(state["btcscript"]), state["btctxid"], int(state["btcvout"]),
                             wif_to_key(a.btc_wif), a.to, preimage=preimage)
        print(json.dumps({"txid": btc("sendrawtransaction", raw)}))
    elif a.cmd == "refund-btc":
        state = json.load(open(a.state))
        raw = btc_spend_htlc(btc, bytes.fromhex(state["btcscript"]), state["btctxid"], int(state["btcvout"]),
                             wif_to_key(state["btcrefundwif"]), a.to, timeout=int(state["btctimeout"]))
        print(json.dumps({"txid": btc("sendrawtransaction", raw)}))
    elif a.cmd == "refund-frc":
        state = json.load(open(a.state))
        raw = frchtlc.spend_htlc(frc, CScript(bytes.fromhex(state["frcscript"])), state["frctxid"], int(state["frcvout"]),
                                 wif_to_key(state["frcrefundwif"]), a.to, timeout=int(state["frctimeout"]))
        print(json.dumps({"txid": frc("sendrawtransaction", raw)}))


if __name__ == "__main__":
    main()
