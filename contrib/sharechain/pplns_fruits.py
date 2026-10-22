"""Reference PPLNS for the fruit-style share chain (doc/sharechain-uncles.md §4, §5).

chain: list of chain shares, oldest first; each = dict(miner, target, fruits=[dict(miner, target), ...]).
The chain share itself is also a fruit (2-for-1): its own work counts once, in its own fruit list
(the template builder lists itself? No: a chain share is credited as a fruit by the NEXT chain
share that lists it, like any share). Here, for the reference, every chain share's `fruits` list
contains every share (including the chain share before it) recorded by it.

credits(anchor, W): walk chain shares from `anchor` backwards; for each, add work(f) for each
listed fruit to credit[f.miner]; stop when accumulated >= W (the crossing chain share counted in full).
payouts(credit, TOTAL, builder): floor split, remainder to builder, dust drop (one pass), cap to
MAX_PAYEES largest (one pass), canonical order by (version, program) = by scriptPubKey bytes.
"""
import json, hashlib, random
TWO256 = 1 << 256
DUST = 1000; MAX_PAYEES = 100

def work(target): return TWO256 // (target + 1)

def compact_to_target(bits):
    exp = bits >> 24; mant = bits & 0x7fffff
    return mant << (8 * (exp - 3)) if exp > 3 else mant >> (8 * (3 - exp))

def scaled(credit):
    # the integer rule both implementations follow: shift credits so the sum fits in 190 bits
    tot = sum(credit.values()); k = max(0, tot.bit_length() - 190)
    return {m: v >> k for m, v in credit.items()}

def credits(chain, anchor_idx, window_work):
    credit = {}; acc = 0; i = anchor_idx
    while i >= 0 and acc < window_work:
        for f in chain[i]['fruits']:
            w = work(f['target']); credit[f['miner']] = credit.get(f['miner'], 0) + w; acc += w
        i -= 1
    return credit

def payouts(credit, total, builder, dust=DUST, max_payees=MAX_PAYEES):
    credit = scaled(credit)
    def split(c):
        tot = sum(c.values()) or 1
        outs = {m: total * v // tot for m, v in c.items()}
        rem = total - sum(outs.values())
        if builder in outs or rem > 0: outs[builder] = outs.get(builder, 0) + rem
        return outs
    outs = split(credit)
    small = [m for m, v in outs.items() if v < dust and m != builder]
    if small:
        for m in small: credit.pop(m)
        outs = split(credit)
    if len(outs) > max_payees:
        keep = sorted(credit, key=lambda m: (-credit[m], m))[:max_payees]
        credit = {m: credit[m] for m in keep}; outs = split(credit)
    return [(m, outs[m]) for m in sorted(outs)]

def vectors():
    random.seed(11)
    miners = [bytes([i + 1]) * 20 for i in range(8)]      # 20-byte witness v0 programs
    hx = lambda b: b.hex()
    cases = []
    for case, (n, per, win) in enumerate([(5, 3, 8), (20, 6, 40), (60, 6, 120), (200, 8, 600)]):
        exp = 0x1d - case                                   # compact exponent; mantissa random -> exact compact targets
        chain = []
        for i in range(n):
            fr = []
            for _ in range(random.randint(1, per)):
                bits = (exp << 24) | random.randrange(0x008000, 0x7fffff)
                fr.append(dict(miner=hx(random.choice(miners)), bits=bits))
            cbits = (exp << 24) | random.randrange(0x008000, 0x7fffff)
            chain.append(dict(miner=hx(random.choice(miners)), bits=cbits, fruits=fr))
        W = win * work(compact_to_target((exp << 24) | 0x400000)); total = 9_536_743_164 + random.randrange(0, 20_000_000)
        builder = hx(random.choice(miners))
        c = credits([dict(miner=s['miner'], target=compact_to_target(s['bits']), fruits=[dict(miner=f['miner'], target=compact_to_target(f['bits'])) for f in s['fruits']]) for s in chain], n - 1, W)
        outs = payouts(c, total, builder)
        assert sum(v for _, v in outs) == total
        cases.append(dict(chain=chain, window_work=hex(W), total=total, builder=builder, dust=DUST, max_payees=MAX_PAYEES,
                          outputs=[[m, v] for m, v in outs]))
    return cases

if __name__ == '__main__':
    cs = vectors()
    json.dump(cs, open('sharechain_pplns.json', 'w'), separators=(',', ':'))
    for c in cs:
        print(f"chain={len(c['chain']):3d} fruits={sum(len(s['fruits']) for s in c['chain']):4d} total={c['total']} outputs={len(c['outputs'])} digest={hashlib.sha256(json.dumps(c['outputs']).encode()).hexdigest()[:16]}")
