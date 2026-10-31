#!/usr/bin/env python3
"""Sell share chain credits for FRC on the Freicoin chain, with a hash lock on both sides
(doc/sharechain-transfers.md §4.2).  No lightning, no bitcoin.

The market maker M knows the preimage P and locks FRC first, for a long time; the seller X
locks its credits for a short one; M's claim of the credits reveals P in the share chain,
and X spends the FRC with it:

  1. M: offer      fund a P2WSH HTLC: X's key with P, or M's key after Freicoin height T.
                   Writes a state file (keep it: it holds P and the refund key) and prints
                   the offer for X.
  2. X: lock       check the HTLC output (script, amount, confirmations, time left before T),
                   then lock its credits to M with hashlock H = SHA256(P) for E chain shares.
  3. M: claim      claim the locked credits with P (the claim reveals P).
  4. X: redeem     read P from listsharetxs.claimed and spend the HTLC to its address.
  5. M: refund     if X never locked, take the FRC back once height T has passed.

Safety: X locks only while T is far enough ahead that it can spend the HTLC after M's claim
(--min-blocks); M claims only while the lock has --min-left chain shares to run.  M carries
the race of §4 (a block found while the credits are locked pays X) and prices it.

Keys are plain WIF keys outside any wallet (newkey.py makes them); the HTLC spends are built
and signed here with the functional-test framework's Freicoin transaction code.
"""
import argparse
import hashlib
import json
import os
import secrets
import sys
from decimal import Decimal

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "test", "functional"))
from test_framework.address import address_to_scriptpubkey, base58_to_byte, script_to_p2wsh  # noqa: E402
from test_framework.key import ECKey  # noqa: E402
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut  # noqa: E402
from test_framework.script import (  # noqa: E402
    OP_CHECKLOCKTIMEVERIFY, OP_CHECKSIG, OP_DROP, OP_ELSE, OP_ENDIF, OP_EQUALVERIFY, OP_IF, OP_SHA256,
    SIGHASH_ALL, CScript, SegwitV0SignatureHash,
)
from test_framework.script_util import script_to_p2wsh_script, script_to_witness  # noqa: E402

from common import WORK_UNIT, Rpc  # noqa: E402

DEFAULT_FEE = 5000  # kria; a redeem or refund is ~250 vbytes


# ---- script and keys ---------------------------------------------------------------------
def htlc_script(hashlock: bytes, seller_pub: bytes, maker_pub: bytes, timeout: int) -> CScript:
    """X's key with the preimage of `hashlock`, or M's key once the Freicoin height is past
    `timeout` (absolute, CHECKLOCKTIMEVERIFY).  Unlike bitcoin's, Freicoin's
    CHECKLOCKTIMEVERIFY drops its argument in witness scripts: no OP_DROP after it."""
    assert len(hashlock) == 32 and len(seller_pub) == 33 and len(maker_pub) == 33
    return CScript([OP_IF, OP_SHA256, hashlock, OP_EQUALVERIFY, seller_pub,
                    OP_ELSE, timeout, OP_CHECKLOCKTIMEVERIFY, maker_pub,
                    OP_ENDIF, OP_CHECKSIG])


def htlc_address(script: CScript, main: bool) -> str:
    return script_to_p2wsh(script, main=main)


def wif_to_key(wif: str) -> ECKey:
    payload, version = base58_to_byte(wif)
    assert version in (128, 239), "not a WIF key"
    compressed = len(payload) == 33 and payload[-1] == 1
    key = ECKey()
    key.set(payload[:32], compressed=compressed)
    assert key.is_valid and compressed, "a compressed WIF key is required"
    return key


def new_key() -> ECKey:
    key = ECKey()
    key.set(secrets.token_bytes(32), compressed=True)
    return key


def key_to_wif(key: ECKey, main: bool) -> str:
    from test_framework.address import byte_to_base58
    return byte_to_base58(key.get_bytes() + b"\x01", 128 if main else 239)


def is_main(rpc) -> bool:
    return rpc("getblockchaininfo")["chain"] == "main"


def to_kria(frc) -> int:
    return int(Decimal(str(frc)) * COIN)


# ---- spending the HTLC -------------------------------------------------------------------
def spend_htlc(rpc, script: CScript, txid: str, vout: int, key: ECKey, to_address: str,
               preimage: bytes = None, timeout: int = None, fee: int = DEFAULT_FEE) -> str:
    """Build and sign a spend of the HTLC output: the redeem branch with `preimage`, or the
    refund branch (nLockTime = `timeout`).  Returns the raw transaction hex."""
    utxo = rpc("gettxout", txid, vout, True)
    if not utxo:
        raise RuntimeError("the HTLC output is spent or unknown")
    if utxo["scriptPubKey"]["hex"] != script_to_p2wsh_script(script).hex():
        raise RuntimeError("that output is not this HTLC")
    value = to_kria(utxo["value"])        # at its reference height: what the signature covers
    available = to_kria(utxo["amount"])   # time-adjusted to the next block's height
    refheight = int(utxo["refheight"])
    next_height = rpc("getblockcount") + 1
    tx = CTransaction()
    refund = preimage is None
    tx.vin = [CTxIn(COutPoint(int(txid, 16), vout), b"", 0xfffffffe if refund else 0xffffffff)]
    if available - fee <= 0:
        raise RuntimeError("the HTLC is worth less than the fee")
    tx.vout = [CTxOut(available - fee, address_to_scriptpubkey(to_address))]
    tx.nLockTime = timeout if refund else 0
    tx.lock_height = next_height
    tx.wit.vtxinwit = [CTxInWitness()]
    sighash = SegwitV0SignatureHash(script, tx, 0, SIGHASH_ALL, value, refheight)
    sig = key.sign_ecdsa(sighash) + bytes([SIGHASH_ALL])
    if refund:
        tx.wit.vtxinwit[0].scriptWitness.stack = [sig, b"", script_to_witness(script), b""]
    else:
        tx.wit.vtxinwit[0].scriptWitness.stack = [sig, preimage, b"\x01", script_to_witness(script), b""]
    tx.rehash()
    return tx.serialize().hex()


# ---- the five steps ----------------------------------------------------------------------
def make_offer(rpc, seller_pub: bytes, frc: Decimal, timeout_blocks: int, units: float, credits_to: str):
    """M funds the HTLC from its node's wallet.  Returns (offer for X, private state for M)."""
    main = is_main(rpc)
    preimage = secrets.token_bytes(32)
    hashlock = hashlib.sha256(preimage).digest()
    refund_key = new_key()
    timeout = rpc("getblockcount") + timeout_blocks
    script = htlc_script(hashlock, seller_pub, refund_key.get_pubkey().get_bytes(), timeout)
    address = htlc_address(script, main)
    txid = rpc("sendtoaddress", address, str(frc))
    spk = script_to_p2wsh_script(script).hex()
    vout = next(o["n"] for o in rpc("gettransaction", txid, True, True)["decoded"]["vout"]
                if o["scriptPubKey"]["hex"] == spk)
    offer = {"txid": txid, "vout": vout, "frc": str(frc), "hashlock": hashlock.hex(), "timeout": timeout,
             "sellerpubkey": seller_pub.hex(), "makerpubkey": refund_key.get_pubkey().get_bytes().hex(),
             "script": script.hex(), "address": address, "creditsto": credits_to, "units": units}
    state = dict(offer, preimage=preimage.hex(), refundwif=key_to_wif(refund_key, main))
    return offer, state


def check_offer(rpc, offer, seller_key: ECKey, min_blocks: int, min_conf: int):
    """X's checks before locking credits.  Returns the HTLC script."""
    script = htlc_script(bytes.fromhex(offer["hashlock"]), seller_key.get_pubkey().get_bytes(),
                         bytes.fromhex(offer["makerpubkey"]), int(offer["timeout"]))
    if script.hex() != offer["script"]:
        raise RuntimeError("the offer's script is not the HTLC for our key, its hashlock and timeout")
    utxo = rpc("gettxout", offer["txid"], int(offer["vout"]), True)
    if not utxo:
        raise RuntimeError("the HTLC output does not exist (or is spent)")
    if utxo["scriptPubKey"]["hex"] != script_to_p2wsh_script(script).hex():
        raise RuntimeError("the output is not paid to that HTLC")
    if Decimal(str(utxo["amount"])) < Decimal(offer["frc"]) * Decimal("0.999"):
        raise RuntimeError(f"the HTLC holds {utxo['amount']} FRC, the offer says {offer['frc']}")
    if utxo["confirmations"] < min_conf:
        raise RuntimeError(f"the HTLC has {utxo['confirmations']} confirmations, we want {min_conf}")
    left = int(offer["timeout"]) - rpc("getblockcount")
    if left < min_blocks:
        raise RuntimeError(f"only {left} Freicoin blocks before the maker can take the FRC back; we want {min_blocks}")
    return script


def lock_credits(rpc, offer, credit_wif: str, expiry: int, maxheight=None):
    """X locks its credits to the maker's address under the offer's hashlock."""
    units = float(offer.get("units") or 0)
    args = [credit_wif, offer["creditsto"], units]
    args += [maxheight if maxheight is not None else rpc("getsharechaininfo")["height"], 0, offer["hashlock"], expiry]
    return rpc("transfershares", *args)


def find_locked(rpc, state):
    for p in rpc("listsharetxs")["pending"]:
        if p.get("hashlock") == state["hashlock"] and p["to"] == state["creditsto"]:
            return p
    return None


def claim_credits(rpc, state, min_left: int):
    """M claims the credits locked under its hashlock; this reveals the preimage."""
    p = find_locked(rpc, state)
    if not p:
        return None, "no pending transfer under our hashlock yet"
    left = p["expiry"] - rpc("getsharechaininfo")["height"]
    if left < min_left:
        return None, f"only {left} chain shares before the lock lapses; not claiming"
    want = float(state.get("units") or 0) * WORK_UNIT
    if want and p["work"] < want * 0.999:
        return None, f"the lock holds {p['work'] / WORK_UNIT:.3f} units, the offer was for {want / WORK_UNIT:.3f}"
    return rpc("claimshares", p["txid"], state["preimage"]), None


def revealed_preimage(rpc, transfer_txid: str, hashlock: str):
    """The preimage M's claim revealed: from the window's claims, or a claim still in the pool."""
    lists = rpc("listsharetxs")
    for c in lists.get("claimed", []):
        if c["transfer"] == transfer_txid:
            return c["preimage"]
    for t in lists["pool"]:
        if t.get("type") == "claim" and t.get("transfer") == transfer_txid and t.get("preimage"):
            if hashlib.sha256(bytes.fromhex(t["preimage"])).hexdigest() == hashlock:
                return t["preimage"]
    return None


# ---- command line ------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rpc", required=True, help="freicoind RPC URL (with /wallet/<name> for the maker's offer)")
    ap.add_argument("--rpcuser", required=True)
    ap.add_argument("--rpcpassword", required=True)
    sub = ap.add_subparsers(dest="cmd", required=True)
    o = sub.add_parser("offer", help="M: fund the HTLC and print the offer")
    o.add_argument("--seller-pubkey", required=True, help="the seller's compressed public key (hex), for the FRC")
    o.add_argument("--frc", required=True, type=Decimal, help="FRC to pay")
    o.add_argument("--units", type=float, default=0, help="units of work bought (0: everything the seller locks)")
    o.add_argument("--credits-to", required=True, help="our address that receives the credits")
    o.add_argument("--timeout-blocks", type=int, default=36, help="Freicoin blocks until we can take the FRC back")
    o.add_argument("--state", required=True, help="file to keep the preimage and refund key in")
    lk = sub.add_parser("lock", help="X: check the offer, lock credits to the maker")
    lk.add_argument("--offer", required=True)
    lk.add_argument("--frc-wif", required=True, help="the key the HTLC pays (its public key went into the offer)")
    lk.add_argument("--credit-wif", required=True, help="the key of the address that owns the credits")
    lk.add_argument("--expiry", type=int, default=30, help="chain shares the credits stay locked")
    lk.add_argument("--min-blocks", type=int, default=12, help="Freicoin blocks that must remain before the maker's refund")
    lk.add_argument("--min-conf", type=int, default=1)
    c = sub.add_parser("claim", help="M: claim the locked credits (reveals the preimage)")
    c.add_argument("--state", required=True)
    c.add_argument("--min-left", type=int, default=5)
    r = sub.add_parser("redeem", help="X: take the FRC with the revealed preimage")
    r.add_argument("--offer", required=True)
    r.add_argument("--transfer", required=True, help="the txid `lock` printed")
    r.add_argument("--frc-wif", required=True)
    r.add_argument("--to", required=True, help="where the FRC go")
    r.add_argument("--fee", type=int, default=DEFAULT_FEE, help="kria")
    f = sub.add_parser("refund", help="M: take the FRC back after the timeout")
    f.add_argument("--state", required=True)
    f.add_argument("--to", required=True)
    f.add_argument("--fee", type=int, default=DEFAULT_FEE, help="kria")
    a = ap.parse_args()
    rpc = Rpc(a.rpc, a.rpcuser, a.rpcpassword)

    if a.cmd == "offer":
        offer, state = make_offer(rpc, bytes.fromhex(a.seller_pubkey), a.frc, a.timeout_blocks, a.units, a.credits_to)
        with open(a.state, "w") as fh:
            json.dump(state, fh, indent=1)
        os.chmod(a.state, 0o600)
        print(json.dumps(offer, indent=1))
    elif a.cmd == "lock":
        offer = json.load(open(a.offer))
        check_offer(rpc, offer, wif_to_key(a.frc_wif), a.min_blocks, a.min_conf)
        print(json.dumps({"transfer": lock_credits(rpc, offer, a.credit_wif, a.expiry)}))
    elif a.cmd == "claim":
        state = json.load(open(a.state))
        txid, err = claim_credits(rpc, state, a.min_left)
        if err:
            sys.exit(err)
        print(json.dumps({"claim": txid}))
    elif a.cmd == "redeem":
        offer = json.load(open(a.offer))
        preimage = revealed_preimage(rpc, a.transfer, offer["hashlock"])
        if not preimage:
            sys.exit("the maker has not claimed yet: no preimage")
        hexed = spend_htlc(rpc, CScript(bytes.fromhex(offer["script"])), offer["txid"], int(offer["vout"]),
                           wif_to_key(a.frc_wif), a.to, preimage=bytes.fromhex(preimage), fee=a.fee)
        print(json.dumps({"txid": rpc("sendrawtransaction", hexed)}))
    elif a.cmd == "refund":
        state = json.load(open(a.state))
        hexed = spend_htlc(rpc, CScript(bytes.fromhex(state["script"])), state["txid"], int(state["vout"]),
                           wif_to_key(state["refundwif"]), a.to, timeout=int(state["timeout"]), fee=a.fee)
        print(json.dumps({"txid": rpc("sendrawtransaction", hexed)}))


if __name__ == "__main__":
    main()
