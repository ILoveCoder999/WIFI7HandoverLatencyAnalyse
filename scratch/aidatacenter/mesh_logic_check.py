#!/usr/bin/env python3
"""Verify the 17x17 2D-mesh + DOR routing logic before generating ns-3 code."""
import networkx as nx

A = 17  # 17x17 = 289 hosts, degree = 16+16 = 32

def node_id(r, c): return r * A + c
def coord(nid): return (nid // A, nid % A)

# build 2D mesh: each host connects all same-row + all same-col hosts
G = nx.Graph()
for r in range(A):
    for c in range(A):
        G.add_node(node_id(r, c))
for r in range(A):
    for c in range(A):
        for c2 in range(A):
            if c2 != c: G.add_edge(node_id(r,c), node_id(r,c2))
        for r2 in range(A):
            if r2 != r: G.add_edge(node_id(r,c), node_id(r2,c))

degs = [d for _, d in G.degree()]
print(f"2D mesh {A}x{A}: {G.number_of_nodes()} hosts, {G.number_of_edges()} links")
print(f"  degree: min {min(degs)}, max {max(degs)}  (expect 32)")
print(f"  diameter: {nx.diameter(G)}  (expect 2 link-segments = 1 relay hop)")

# DOR routing: row-first
def dor(s, d):
    (rs,cs),(rd,cd) = coord(s), coord(d)
    if s==d: return [s]
    if rs==rd or cs==cd: return [s,d]           # same row/col: 0 relay
    return [s, node_id(rs,cd), d]               # via row-intersection: 1 relay

# verify DOR matches shortest path & relay count for ALL pairs
maxrelay=0; bad=0
for s in range(A*A):
    for d in range(A*A):
        if s==d: continue
        p = dor(s,d)
        relay = len(p)-2
        maxrelay = max(maxrelay, relay)
        # check each consecutive pair is actually an edge
        for i in range(len(p)-1):
            if not G.has_edge(p[i],p[i+1]): bad+=1
print(f"  DOR check: max relay hops = {maxrelay} (expect 1), invalid edges = {bad} (expect 0)")
print("  LOGIC OK" if maxrelay==1 and bad==0 else "  LOGIC FAIL")
