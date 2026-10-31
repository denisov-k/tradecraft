#!/usr/bin/env python3
"""Regtest stand for atomicswap.py: a bitcoind and a freicoind (see stand.sh), the whole swap,
then both refunds.  Usage: stand.py <btc rpc url> <frc rpc url> <user> <password>"""
import json
import os
import sys
from decimal import Decimal

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import atomicswap as A  # noqa: E402
from common import Rpc  # noqa: E402

btc_url, frc_url, user, password = sys.argv[1:5]
btc_node, frc_node = Rpc(btc_url, user, password), Rpc(frc_url, user, password)
btc_a = Rpc(btc_url + "/wallet/a", user, password)   # A's bitcoin wallet pays the BTC HTLC
frc_b = Rpc(frc_url + "/wallet/b", user, password)   # B's freicoin wallet pays the FRC HTLC


def mine_btc(n):
    btc_node("generatetoaddress", n, btc_a("getnewaddress"))


def mine_frc(n):
    frc_node("generatetoaddress", n, frc_b("getnewaddress"))


def step(msg):
    print(f"== {msg}", flush=True)


step("setup")
for rpc, name in ((btc_node, "a"), (frc_node, "b")):
    if name not in rpc("listwallets"):
        try:
            rpc("loadwallet", name)
        except Exception:
            rpc("createwallet", name)
mine_btc(101)
mine_frc(110)
print("A BTC", btc_a("getbalance"), " B FRC", frc_b("getbalance"))

b_btc_key = A.new_key()
b_btc_wif = A.frchtlc.key_to_wif(b_btc_key, False)

step("1. A locks BTC")
offer, a_state = A.initiate(btc_a, frc_node, b_btc_key.get_pubkey().get_bytes(), Decimal("0.5"), Decimal("40"), 30)
print(json.dumps({k: offer[k] for k in ("btctxid", "btc", "frc", "btctimeout")}))
mine_btc(1)

step("2. B checks and locks FRC")
try:
    A.participate(btc_node, frc_b, offer, A.new_key(), 10, 20, 1)
    raise SystemExit("FAIL: B accepted an offer for another key")
except RuntimeError as e:
    print("refused as expected:", e)
reply, b_state = A.participate(btc_node, frc_b, offer, b_btc_key, 10, 20, 1)
print(json.dumps({k: reply[k] for k in ("frctxid", "frctimeout")}))
mine_frc(1)

step("3. A takes the FRC (reveals the preimage)")
fscript = A.check_participation(frc_node, a_state, reply, 1)
a_frc_to = frc_b("getnewaddress")  # any address; reuse B's wallet node to watch it
raw = A.frchtlc.spend_htlc(frc_node, fscript, reply["frctxid"], reply["frcvout"], A.wif_to_key(a_state["frcwif"]), a_frc_to,
                           preimage=bytes.fromhex(a_state["preimage"]))
frc_spend = frc_node("sendrawtransaction", raw)
print("A's FRC spend", frc_spend)

step("4. B finds the preimage on the Freicoin chain (mempool), then in a block, and takes the BTC")
p = A.frc_find_preimage(frc_node, b_state["frctxid"], b_state["frcvout"], bytes.fromhex(b_state["hashlock"]), b_state["frcheight"])
assert p and p.hex() == a_state["preimage"], "preimage not found in the mempool"
mine_frc(1)
p = A.frc_find_preimage(frc_node, b_state["frctxid"], b_state["frcvout"], bytes.fromhex(b_state["hashlock"]), b_state["frcheight"])
assert p and p.hex() == a_state["preimage"], "preimage not found in the block"
b_btc_to = btc_a("getnewaddress")
raw = A.btc_spend_htlc(btc_node, bytes.fromhex(b_state["btcscript"]), b_state["btctxid"], b_state["btcvout"], b_btc_key, b_btc_to, preimage=p)
btc_spend = btc_node("sendrawtransaction", raw)
mine_btc(1)
got_btc = btc_node("gettxout", btc_spend, 0)
got_frc = frc_node("gettxout", frc_spend, 0)
assert got_btc and Decimal(str(got_btc["value"])) > Decimal("0.4999"), got_btc
assert got_frc and Decimal(str(got_frc["value"])) > Decimal("39.99"), got_frc
print("swap done: B got", got_btc["value"], "BTC, A got", got_frc["value"], "FRC")

step("5. refunds: A's BTC after its timeout, B's FRC after its timeout")
offer2, a2 = A.initiate(btc_a, frc_node, b_btc_key.get_pubkey().get_bytes(), Decimal("0.2"), Decimal("10"), 5)
mine_btc(1)
reply2, b2 = A.participate(btc_node, frc_b, offer2, b_btc_key, 4, 2, 1)
mine_frc(1)
script2 = bytes.fromhex(a2["btcscript"])
early = A.btc_spend_htlc(btc_node, script2, a2["btctxid"], a2["btcvout"], A.wif_to_key(a2["btcrefundwif"]), btc_a("getnewaddress"), timeout=a2["btctimeout"])
try:
    btc_node("sendrawtransaction", early)
    raise SystemExit("FAIL: bitcoin refund accepted before the timeout")
except RuntimeError as e:
    print("early BTC refund refused:", str(e)[:80])
while btc_node("getblockcount") < a2["btctimeout"]:
    mine_btc(1)
late = A.btc_spend_htlc(btc_node, script2, a2["btctxid"], a2["btcvout"], A.wif_to_key(a2["btcrefundwif"]), btc_a("getnewaddress"), timeout=a2["btctimeout"])
print("BTC refund", btc_node("sendrawtransaction", late))
fscript2 = A.CScript(bytes.fromhex(b2["frcscript"]))
while frc_node("getblockcount") < b2["frctimeout"]:
    mine_frc(1)
raw = A.frchtlc.spend_htlc(frc_node, fscript2, b2["frctxid"], b2["frcvout"], A.wif_to_key(b2["frcrefundwif"]), frc_b("getnewaddress"), timeout=b2["frctimeout"])
print("FRC refund", frc_node("sendrawtransaction", raw))
mine_btc(1)
mine_frc(1)
print("STAND OK")
