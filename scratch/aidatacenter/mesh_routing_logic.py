#!/usr/bin/env python3
"""
Validate the 2D-mesh routing LOGIC (DOR vs Valiant) before writing ns-3 code.
Pod = a x a hosts, row+column fully connected (each dim 1 hop, diameter 2).

DOR (dimension-order): fix row first then column.
  src(rs,cs) -> dst(rd,cd):
    if rs==rd: direct (same row, 1 hop)
    elif cs==cd: direct (same column, 1 hop)
    else: via (rs,cd)  [go along row to dst column, then down column]  2 hops
Valiant: pick random intermediate (ri,ci), DOR src->inter, DOR inter->dst.

We inject a HOTSPOT traffic pattern and measure per-link load to show DOR
concentrates on intersection links while Valiant spreads it.
"""
import random
from collections import defaultdict
random.seed(0)

A = 8  # pod side -> 64 hosts

def dor_path(s, d):
    (rs, cs), (rd, cd) = s, d
    if s == d: return [s]
    if rs == rd or cs == cd:
        return [s, d]              # same row or col: 1 hop
    inter = (rs, cd)               # row-first: go to dst column within my row
    return [s, inter, d]

def valiant_path(s, d):
    (rs, cs), (rd, cd) = s, d
    if s == d: return [s]
    # random intermediate node
    ri, ci = random.randrange(A), random.randrange(A)
    inter = (ri, ci)
    p1 = dor_path(s, inter)
    p2 = dor_path(inter, d)
    return p1[:-1] + p2            # stitch, avoid dup

def links_of(path):
    return [frozenset((path[i], path[i+1])) for i in range(len(path)-1)]

def run(pattern_pairs, router):
    load = defaultdict(int)
    hops = []
    for s, d in pattern_pairs:
        path = router(s, d)
        hops.append(len(path)-1)
        for lk in links_of(path):
            load[lk] += 1
    loads = list(load.values())
    return max(loads), sum(loads)/len(loads), max(hops), sum(hops)/len(hops)

# hotspot pattern: many sources all send to a small set of destinations in one
# corner -> DOR funnels through few intersection links.
nodes = [(r,c) for r in range(A) for c in range(A)]
hot_dsts = [(0,0),(0,1),(1,0),(1,1)]
pairs = []
for s in nodes:
    for d in hot_dsts:
        if s != d: pairs.append((s,d))

print("="*64)
print(f"2D-mesh {A}x{A} ({A*A} hosts) — hotspot pattern, DOR vs Valiant")
print(f"{len(pairs)} flows all targeting a 2x2 corner")
print("="*64)
for name, r in [("DOR (dimension-order)", dor_path),
                ("Valiant (random relay)", valiant_path)]:
    mx, mean, mxh, meanh = run(pairs, r)
    print(f"\n  {name}")
    print(f"    max link load: {mx:4d}   mean link load: {mean:5.1f}   "
          f"imbalance {mx/mean:4.1f}x")
    print(f"    hops: max {mxh}, avg {meanh:.2f}")
print("\n[expected] DOR: high max load (funnels to corner), low hops.")
print("           Valiant: lower max load (spread out), higher hops (~2x).")

print("\n" + "="*64)
print("PERMUTATION pattern (each node -> one distant partner) — Valiant's turf")
print("="*64)
# permutation: node (r,c) sends to (c,r) transpose -> stresses specific dim links
perm_pairs = []
for r in range(A):
    for c in range(A):
        s=(r,c); d=(c,r)
        if s!=d: perm_pairs.append((s,d))
random.seed(1)
for name, rt in [("DOR (dimension-order)", dor_path),
                 ("Valiant (random relay)", valiant_path)]:
    mx, mean, mxh, meanh = run(perm_pairs, rt)
    print(f"\n  {name}")
    print(f"    max link load: {mx:4d}   mean {mean:5.1f}   imbalance {mx/mean:4.1f}x")
    print(f"    hops: max {mxh}, avg {meanh:.2f}")
print("\n[takeaway] On permutation/transpose traffic (no single dst hotspot),")
print("  Valiant spreads the load and LOWERS max link load vs DOR's deterministic")
print("  funneling. This is where Valiant wins; on incast-to-few-dsts it does not.")
