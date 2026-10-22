"""Credit fairness under network delay for three share-crediting rules.

Miners i have hashrate h_i (fraction) and one-way propagation delay d_i (seconds) to/from everyone
else. Shares arrive as a Poisson process with mean spacing T_S over the whole pool. Each share is
created on its miner's CURRENT VIEW of the chain (the global tip as of t - d_i, plus the miner's own
shares). Rules compared:

  A  chain-only (classic p2pool): a share earns credit only if it ends up on the main chain.
     Two shares with the same parent: the earlier-created one wins (first-seen), the other is lost.
  B  uncles (Monero p2pool): a lost share is credited 80% if its parent is within UNCLE_DEPTH of
     the including chain share; the includer gets the other 20%.
  C  fruits (FruitChain-style, proposed): every share is a fruit hanging from the chain share it
     saw; it is credited in full as long as some chain share within R includes it. Chain shares are
     the subset of shares that also meet the chain target (1 in M), reward-neutral.

Output: credited/earned work per miner for each rule. 1.00 = perfectly fair.
"""
import random, sys

def simulate(miners, T_S=10.0, M=6, R=10, UNCLE_DEPTH=3, UNCLE_PENALTY=0.20, hours=48, seed=1):
    random.seed(seed)
    n = len(miners); H = sum(h for h, _ in miners)
    t = 0.0; shares = []           # dict(t, miner, parent, chain)   parent = index of chain tip seen
    chain_tips_by_time = [(0.0, -1)]  # (time, index of chain share) global chain tip history (first-seen rule)
    def tip_at(time):              # global chain tip index as of `time`
        lo, hi = 0, len(chain_tips_by_time) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if chain_tips_by_time[mid][0] <= time: lo = mid
            else: hi = mid - 1
        return chain_tips_by_time[lo][1]
    height = {-1: -1}
    end = hours * 3600
    while t < end:
        t += random.expovariate(1.0 / T_S)
        r = random.random() * H; acc = 0; mi = 0
        for i, (h, _) in enumerate(miners):
            acc += h
            if r <= acc: mi = i; break
        d = miners[mi][1]
        parent = tip_at(t - d)                                   # what this miner has seen so far
        # the miner also knows its OWN recent chain shares even if not yet propagated
        for j in range(len(shares) - 1, -1, -1):
            s = shares[j]
            if s['t'] < t - d: break
            if s['miner'] == mi and s['chain'] and height[j] > height[parent]: parent = j
        is_chain = random.random() < 1.0 / M                      # 2-for-1: 1 in M shares meets the chain target
        shares.append(dict(t=t, miner=mi, parent=parent, chain=is_chain, d=d))
        idx = len(shares) - 1
        height[idx] = height[parent] + 1
        if is_chain:
            # first-seen rule from the global vantage point: a chain share extends the global tip only
            # if it is higher than the current tip (ties go to the earlier one, already there)
            cur = chain_tips_by_time[-1][1]
            if height[idx] > height[cur]:
                chain_tips_by_time.append((t + d, idx))          # becomes everyone's tip after propagation
    # --- main chain = ancestors of the final tip
    main = set(); x = chain_tips_by_time[-1][1]
    while x != -1: main.add(x); x = shares[x]['parent']
    main_heights = {x: height[x] for x in main}
    earned = [0.0] * n; A = [0.0] * n; B = [0.0] * n; C = [0.0] * n
    main_by_h = {height[x]: x for x in main}
    def first_main_after(h_from, h_to, t_vis):
        """first main-chain share with height in (h_from, h_to] created at or after t_vis, else None"""
        for hh in range(h_from + 1, h_to + 1):
            x = main_by_h.get(hh)
            if x is not None and shares[x]['t'] >= t_vis: return x
        return None
    for i, s in enumerate(shares):
        earned[s['miner']] += 1
        p = s['parent']; t_vis = s['t'] + s['d']              # when everyone else has seen it
        if s['chain']:
            if i in main:
                A[s['miner']] += 1; B[s['miner']] += 1
            elif p == -1 or p in main:
                # rule B: uncle if a main share within UNCLE_DEPTH of the fork point includes it
                inc = first_main_after(height[p], height[p] + UNCLE_DEPTH, t_vis)
                if inc is not None:
                    B[s['miner']] += 1 - UNCLE_PENALTY; B[shares[inc]['miner']] += UNCLE_PENALTY
        # rule C: fruit hangs from chain share p; if p was orphaned, fall back to p's parent (one level);
        # credited iff a main chain share within R heights of the hang target was created after the
        # fruit became visible (that share includes it; all chain shares are honest here)
        q = p if (p == -1 or p in main) else shares[p]['parent']
        if (q == -1 or q in main) and first_main_after(height[q], height[q] + R, t_vis) is not None:
            C[s['miner']] += 1
    return earned, A, B, C

def simulate_classic(miners, T_S=10.0, UNCLE_DEPTH=3, UNCLE_PENALTY=0.20, hours=48, seed=1):
    """Every share is a chain share (M=1): classic p2pool (A) and Monero-style uncles (B)."""
    e, A, B, _ = simulate(miners, T_S=T_S, M=1, R=0, UNCLE_DEPTH=UNCLE_DEPTH, UNCLE_PENALTY=UNCLE_PENALTY, hours=hours, seed=seed)
    return e, A, B

if __name__ == '__main__':
    # hashrate fraction, one-way delay in seconds
    miners = [(0.40, 0.3), (0.25, 0.5), (0.15, 1.0), (0.10, 2.0), (0.06, 4.0), (0.04, 8.0)]
    hours = int(sys.argv[1]) if len(sys.argv) > 1 else 72
    e, A, B = simulate_classic(miners, hours=hours)
    _, _, _, C = simulate(miners, M=6, R=10, hours=hours)
    print(f"{'miner':>6} {'hash':>6} {'delay':>6} | {'A chain-only':>12} {'B uncles':>9} {'C fruits':>9}   (credited/earned)")
    for i, (h, d) in enumerate(miners):
        print(f"{i:>6} {h:>6.2f} {d:>5.1f}s | {A[i]/e[i]:>12.3f} {B[i]/e[i]:>9.3f} {C[i]/e[i]:>9.3f}")
    tot = sum(e)
    print(f"lost work overall: A {1-sum(A)/tot:.1%}   B {1-sum(B)/tot:.1%}   C {1-sum(C)/tot:.1%}")
