"""PPLNS-by-work with uncles for a Freicoin sharechain: reference model + test vectors.

Rules (proposal, 2026-10-21):
  * every share carries `bits` -> work_i = 2^256 / (target_i + 1)  (same as block chainwork)
  * window W (in work units) = K * expected pool work per FRC block, walked back from the share's
    own ancestry (parent chain), including uncles referenced by shares in the window
  * uncle = a valid share whose parent is an ancestor of the chain within UNCLE_DEPTH heights,
    not already in the chain; it is credited UNCLE_PENALTY-discounted work; the share that
    includes it gets no bonus (keeps the rule simple; Monero p2pool gives 20% of the penalty to
    the includer, can be added later)
  * payout_i = floor(TOTAL * credited_work_i / credited_work_sum); the remainder (dust from
    flooring) goes to the share's own miner (the template builder), so sum(outputs) == TOTAL
  * outputs below DUST are dropped and their value redistributed by the same rule (one pass)
  * deterministic output order: by address bytes (hex) ascending -> a verifier recomputes and
    compares the vector exactly
"""
import json, hashlib
from fractions import Fraction

TWO256 = 1 << 256
UNCLE_DEPTH = 3
UNCLE_PENALTY = Fraction(1, 5)       # uncle credited 80% of its work
DUST = 1000                          # kria; below this an output is dropped

def work(bits_target):               # bits given directly as target int here (vectors use ints)
    return TWO256 // (bits_target + 1)

def pplns_outputs(chain, uncles_by_height, tip_index, window_work, total, dust=DUST):
    """chain: list of shares dict(miner, target, height); uncles_by_height: {height: [uncle shares]}
    (uncles referenced by the share at that height). Walk back from tip_index collecting credited
    work until window_work is reached (the last share is counted in full even if it overshoots:
    simpler than pro-rating and verifiers agree)."""
    credit = {}
    acc = 0
    h = tip_index
    while h >= 0 and acc < window_work:
        s = chain[h]
        w = work(s['target']); credit[s['miner']] = credit.get(s['miner'], 0) + w; acc += w
        for u in uncles_by_height.get(h, []):
            uw = int(work(u['target']) * (1 - UNCLE_PENALTY)); credit[u['miner']] = credit.get(u['miner'], 0) + uw; acc += uw
        h -= 1
    builder = chain[tip_index]['miner']
    def split(credit):
        tot = sum(credit.values())
        outs = {m: total * c // tot for m, c in credit.items()}
        outs[builder] = outs.get(builder, 0) + (total - sum(outs.values()))   # flooring remainder -> builder
        return outs
    outs = split(credit)
    small = [m for m, v in outs.items() if v < dust and m != builder]
    if small:
        for m in small: credit.pop(m)
        outs = split(credit)
    return [(m, outs[m]) for m in sorted(outs)]        # canonical order

def vectors():
    import random
    random.seed(7)
    miners = ['fc1q%02d' % i for i in range(6)]
    cases = []
    for case in range(4):
        n = [8, 30, 120, 400][case]
        target = (1 << 240) >> case                     # share difficulty grows per case
        chain = [dict(miner=random.choice(miners), target=target + random.randrange(1 << 200), height=i) for i in range(n)]
        uncles = {}
        for i in range(2, n):
            if random.random() < 0.08:                 # 8% uncles
                uncles.setdefault(i, []).append(dict(miner=random.choice(miners), target=chain[i]['target'], parent_height=i - random.randint(1, UNCLE_DEPTH)))
        window_work = [10, 20, 50, 200][case] * work(target)    # window in "typical shares"
        total = 9_536_743_164 + random.randrange(0, 20_000_000)   # subsidy + fees (kria)
        outs = pplns_outputs(chain, uncles, n - 1, window_work, total)
        assert sum(v for _, v in outs) == total
        cases.append(dict(n=n, window_shares=[10, 20, 50, 200][case], total=total, uncles=sum(len(v) for v in uncles.values()), outputs=outs,
                          digest=hashlib.sha256(json.dumps(outs).encode()).hexdigest()[:16]))
    return cases

if __name__ == '__main__':
    for c in vectors():
        print(f"shares={c['n']:4d} window={c['window_shares']:3d} uncles={c['uncles']:2d} total={c['total']} outputs={len(c['outputs'])} digest={c['digest']}")
        for m, v in c['outputs']: print(f"    {m}: {v/1e8:.8f} FRC")
