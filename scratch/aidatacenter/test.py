#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
无交换机数据中心拓扑评估 —— 完整代码汇总
================================================================================
场景: 1250 台服务器, 每台 8 GPU(机内 NVLink 全互连), 每台 32 个机间端口。
目标: 大模型 AI 训练 + 日常数据中心指标。

本文件汇总了我们试过的全部内容, 每个函数都标注了它验证什么、结论如何。
所有数字都是真实跑出来的, 可复现(固定 seed)。

诚实结论备忘(供你写作时参考, 别改成相反的):
  - Jellyfish(随机31-正则)在 APL/对分带宽/热点比/调度效率上 全面领先或打平。
  - 双层结构化拓扑唯一干净胜出项: 路由表大小(95 vs 1250), 即支持 O(直径) 坐标路由。
  - 结构化全局连边能把 A2A 热点比从 2.48 压到 2.17, 但仍不及 Jellyfish 的 1.19。
  - de Bruijn 全局骨架更差(热点比 3.2+), 已否决。
  - 边着色调度效率: 结构化 0.904 vs Jellyfish 0.941 —— 结构化并未胜出。
  - 真正有价值的发现: L=15(128-GPU group) 是端口分配最优点。

运行: pip install networkx numpy scipy && python3 topology_suite.py
================================================================================
"""
import itertools, math
import numpy as np
import networkx as nx

N = 1250
PORTS = 32
SEED = 7
rng = np.random.default_rng(SEED)

# ============================ 度量函数 ============================ #
def basic_metrics(G):
    """精确 diameter + APL, 全源 BFS。"""
    total = diam = cnt = 0
    for s in G.nodes():
        for _, dist in nx.single_source_shortest_path_length(G, s).items():
            if dist > 0:
                total += dist; cnt += 1; diam = max(diam, dist)
    return diam, total / cnt

def algebraic_connectivity(G):
    """Fiedler value, 对分带宽代理。"""
    L = nx.laplacian_matrix(G).astype(float)
    try:
        from scipy.sparse.linalg import eigsh
        return float(sorted(eigsh(L, k=2, which='SM', return_eigenvectors=False))[1])
    except Exception:
        return float('nan')

def all_to_all_link_load(G):
    """All-to-All 链路负载 = edge betweenness。返回 (max, mean)。"""
    bc = nx.edge_betweenness_centrality(G, normalized=False)
    v = list(bc.values())
    return max(v), float(np.mean(v))

# ============================ 拓扑构造 ============================ #
def build_jellyfish(N, deg):
    """Jellyfish (NSDI 2014): 随机 d-正则图。强基线。"""
    if (N * deg) % 2: deg -= 1
    return nx.random_regular_graph(deg, N, seed=2)

def build_random_global(N, ports, L):
    """双层 + 随机全局连边(最初版本)。"""
    Gp = ports - L
    if Gp < 1 or L < 1: return None, None, None
    gsize = L + 1; ngroups = math.ceil(N / gsize)
    G = nx.Graph(); G.add_nodes_from(range(N))
    groups = [list(range(i*gsize, min((i+1)*gsize, N))) for i in range(ngroups)]
    nmap = {}
    for gi, grp in enumerate(groups):
        for nd in grp: nmap[nd] = gi
        for a, b in itertools.combinations(grp, 2): G.add_edge(a, b)
    for gi, grp in enumerate(groups):
        for node in grp:
            tg = [x for x in range(ngroups) if x != gi]; rng.shuffle(tg); placed = 0
            for t in tg:
                if placed >= Gp: break
                for c in groups[t]:
                    if G.degree(c) < ports and G.degree(node) < ports and not G.has_edge(node, c):
                        G.add_edge(node, c); placed += 1; break
    return G, groups, nmap

def build_structured_global(N, ports, L):
    """双层 + 结构化均匀全局连边(改进版, 想法一)。热点比最低的方案。"""
    Gp = ports - L
    if Gp < 1 or L < 1: return None, None, None
    gsize = L + 1; ngroups = math.ceil(N / gsize)
    G = nx.Graph(); G.add_nodes_from(range(N))
    groups = [list(range(i*gsize, min((i+1)*gsize, N))) for i in range(ngroups)]
    nmap = {}
    for gi, grp in enumerate(groups):
        for nd in grp: nmap[nd] = gi
        for a, b in itertools.combinations(grp, 2): G.add_edge(a, b)
    for gi, grp in enumerate(groups):
        for ki, node in enumerate(grp):
            for p in range(Gp):
                slot = ki*Gp + p; tg = (gi + 1 + slot) % ngroups
                if tg == gi: tg = (tg + 1) % ngroups
                for c in sorted(groups[tg], key=lambda c: G.degree(c)):
                    if G.degree(c) < ports and G.degree(node) < ports and not G.has_edge(node, c):
                        G.add_edge(node, c); break
    return G, groups, nmap

def build_debruijn_global(N, ports, L):
    """双层 + de Bruijn 组间骨架(想法二)。已验证更差, 保留供对比。"""
    Gp = ports - L; gsize = L + 1; ngroups = math.ceil(N / gsize); b = Gp
    G = nx.Graph(); G.add_nodes_from(range(N))
    groups = [list(range(i*gsize, min((i+1)*gsize, N))) for i in range(ngroups)]
    nmap = {}
    for gi, grp in enumerate(groups):
        for nd in grp: nmap[nd] = gi
        for a, b2 in itertools.combinations(grp, 2): G.add_edge(a, b2)
    for gi, grp in enumerate(groups):
        for node in grp:
            placed = 0
            for c in range(b):
                if placed >= Gp: break
                tg = (gi*b + c) % ngroups
                if tg == gi: continue
                for cc in sorted(groups[tg], key=lambda x: G.degree(x)):
                    if G.degree(cc) < ports and G.degree(node) < ports and not G.has_edge(node, cc):
                        G.add_edge(node, cc); placed += 1; break
    return G, groups, nmap

# ====================== AI 训练专属指标 ====================== #
def ring_purity(G, groups=None):
    """All-Reduce ring 纯1跳比例。"""
    if groups is not None:
        ring = []
        for i, grp in enumerate(groups):
            ring.extend(grp if i % 2 == 0 else grp[::-1])
    else:
        nodes = list(G.nodes()); vis = {0}; ring = [0]; cur = 0
        while len(vis) < len(nodes):
            nb = [n for n in G.neighbors(cur) if n not in vis]
            nxt = nb[0] if nb else [n for n in nodes if n not in vis][0]
            ring.append(nxt); vis.add(nxt); cur = nxt
    pure = sum(1 for i in range(len(ring)) if G.has_edge(ring[i], ring[(i+1) % len(ring)]))
    return pure, len(ring)

def connectivity_under_failure(G, frac, trials=20):
    """随机节点故障下最大连通分量占比。"""
    n = G.number_of_nodes(); keep = int(n*(1-frac)); ratios = []
    for _ in range(trials):
        nodes = list(G.nodes()); rng.shuffle(nodes)
        H = G.subgraph(set(nodes[:keep]))
        if H.number_of_nodes() == 0: ratios.append(0); continue
        ratios.append(max(len(c) for c in nx.connected_components(H)) / keep)
    return float(np.mean(ratios))

def group_multigraph(G, nmap, ngroups):
    M = nx.MultiGraph(); M.add_nodes_from(range(ngroups))
    for u, v in G.edges():
        if nmap[u] != nmap[v]: M.add_edge(nmap[u], nmap[v])
    return M

def fast_edge_coloring(M):
    """内存友好的顺序边着色, 返回 (轮数, 下界=最大度)。
    轮数越接近下界 = 调度效率越高。"""
    color_at = {v: set() for v in M.nodes()}; maxcolor = -1
    for u, v in M.edges():
        used = color_at[u] | color_at[v]; c = 0
        while c in used: c += 1
        color_at[u].add(c); color_at[v].add(c); maxcolor = max(maxcolor, c)
    maxdeg = max((d for _, d in M.degree()), default=0)
    return maxcolor + 1, maxdeg

# ============================ 主流程 ============================ #
def main():
    print("="*68)
    print(f"设定: N={N} 服务器, {PORTS} 机间端口/台, seed={SEED}")
    print("="*68)

    print("\n[基线] Jellyfish 随机 31-正则")
    Gj = build_jellyfish(N, 31)
    dj, aj = basic_metrics(Gj); fj = algebraic_connectivity(Gj); mxj, mnj = all_to_all_link_load(Gj)
    print(f"  diam={dj} APL={aj:.3f} 代数连通度={fj:.3f} A2A热点比={mxj/mnj:.2f}")

    print("\n[端口分配扫描] 三种全局连边规则, 比 A2A 热点比")
    print(f"  {'规则':>10}{'L':>4}{'Gp':>4}{'组大小':>6}{'diam':>5}{'APL':>7}{'热点比':>7}")
    for name, fn in [("随机", build_random_global),
                     ("结构化", build_structured_global),
                     ("deBruijn", build_debruijn_global)]:
        for L in [11, 15, 19]:
            G, groups, nmap = fn(N, PORTS, L)
            if G is None or not nx.is_connected(G):
                print(f"  {name:>10} L={L} 无效"); continue
            d, a = basic_metrics(G); mx, mn = all_to_all_link_load(G)
            print(f"  {name:>10}{L:>4}{PORTS-L:>4}{L+1:>6}{d:>5}{a:>7.3f}{mx/mn:>7.2f}")

    print("\n[最优端口分配] 结构化, L=15 (16台/组 = 128 GPU)")
    Gs, groups, smap = build_structured_global(N, PORTS, 15)
    ng = len(groups)

    print("\n[AI 指标] 结构化 L=15 vs Jellyfish")
    ps, ls = ring_purity(Gs, groups); pj, lj = ring_purity(Gj, None)
    print(f"  AllReduce ring 纯1跳: 结构化 {100*ps/ls:.1f}% | Jellyfish {100*pj/lj:.1f}%")
    print(f"  路由表/节点:          结构化 ~{ng+16} 条 | Jellyfish {N} 条  <-- 唯一干净胜出项")
    for f in [0.05, 0.10, 0.20]:
        print(f"  故障{int(f*100):>2}%连通: 结构化 {connectivity_under_failure(Gs,f):.3f} | "
              f"Jellyfish {connectivity_under_failure(Gj,f):.3f}")

    print("\n[无冲突调度] group级 A2A 边着色效率 (越高越可静态调度)")
    Ms = group_multigraph(Gs, smap, ng)
    rs, mds = fast_edge_coloring(Ms)
    jmap = {nd: nd // 16 for nd in Gj.nodes()}; ngj = max(jmap.values()) + 1
    Mj = group_multigraph(Gj, jmap, ngj)
    rj, mdj = fast_edge_coloring(Mj)
    print(f"  结构化:    轮数={rs} 下界={mds} 效率={mds/rs:.3f}")
    print(f"  Jellyfish: 轮数={rj} 下界={mdj} 效率={mdj/rj:.3f}")

    print("\n" + "="*68)
    print("诚实结论: 纯拓扑指标上 Jellyfish 全面领先或打平。")
    print("结构化唯一真实优势 = 确定性坐标路由(路由表 95 vs 1250)。")
    print("写论文请围绕'可部署性/可路由性'这个真实优势, 不要编造拓扑胜出。")
    print("="*68)

if __name__ == "__main__":
    main()
