#!/usr/bin/env python3
"""
latency_over_distance.py — reproduce the "Latency over distance" figure:

    top panel    : Mean latency vs distance from the AP
    bottom panel : 99th and 99.9th percentile vs distance from the AP
    x-axis       : distance from AP (m)      y-axis : latency (microseconds)

Intended for a SINGLE-AP characterisation (STA moving away from one AP, no
handover). Distance is |STA_x - ap_x| at the packet's first transmission, so
the outbound and return legs are pooled by distance.

USAGE
-----
    python latency_over_distance.py --log handover_sta_log.json --ap-x 0 \
        --dmax 50 --binwidth 1 --outfile latency_over_distance.png

Multiple shard logs may be passed and are pooled:
    python latency_over_distance.py --log results/.../sta_*shard*.json --ap-x 0
"""
import argparse, json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

try:
    import orjson
    _loads = orjson.loads
except ImportError:
    _loads = json.loads


class Buffer:
    def __init__(self, dtype=np.float32, cap=1 << 20):
        self._a = np.empty(cap, dtype=dtype); self._n = 0
    def append(self, v):
        if self._n == self._a.size:
            self._a = np.resize(self._a, self._a.size * 2)
        self._a[self._n] = v; self._n += 1
    def array(self):
        return self._a[:self._n]


def iter_records(path):
    buf = ""
    with open(path) as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line == "[" or line == "]":
                continue
            if line.startswith("["): line = line[1:].strip()
            if line.endswith("]"): line = line[:-1].strip()
            if line.endswith(","): line = line[:-1].strip()
            if not line:
                continue
            cand = (buf + line) if buf else line
            try:
                o = _loads(cand); buf = ""; yield o
            except (json.JSONDecodeError, ValueError):
                buf = cand + " "


def collect(path, ap_x):
    it = iter_records(path)
    try:
        next(it)                      # skip the config header
    except StopIteration:
        return np.array([]), np.array([])
    dist, lat = Buffer(), Buffer()
    for rec in it:
        if not isinstance(rec, dict) or "latency" not in rec:
            continue
        txs = rec.get("transmissions") or []
        if not txs:
            continue
        pos = txs[0].get("position")
        if pos is None:
            continue
        dist.append(abs(float(pos[0]) - ap_x))
        lat.append(float(rec["latency"]) / 1e3)        # ns -> microseconds
    return dist.array(), lat.array()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", nargs="+", required=True, help="STA log(s) to pool")
    ap.add_argument("--ap-x", type=float, default=0.0, help="AP x position (m)")
    ap.add_argument("--binwidth", type=float, default=1.0, help="distance bin (m)")
    ap.add_argument("--dmax", type=float, default=50.0, help="max distance (m)")
    ap.add_argument("--title", default="Latency over distance\n(No interferer)")
    ap.add_argument("--outfile", default="latency_over_distance.png")
    ap.add_argument("--csv", default=None, help="optional CSV dump of the curves")
    args = ap.parse_args()

    Ds, Ls = [], []
    for p in args.log:
        d, l = collect(p, args.ap_x)
        Ds.append(d); Ls.append(l)
    D = np.concatenate(Ds); L = np.concatenate(Ls)
    print(f"pooled {L.size} packets from {len(args.log)} log(s)")

    edges = np.arange(0, args.dmax + args.binwidth, args.binwidth)
    nb = len(edges) - 1
    idx = np.clip(np.floor(D / args.binwidth).astype(np.int64), 0, nb - 1)
    centers = edges[:-1] + args.binwidth / 2.0

    mean = np.full(nb, np.nan); p99 = np.full(nb, np.nan); p999 = np.full(nb, np.nan)
    count = np.zeros(nb, int)
    order = np.argsort(idx, kind="stable")
    idxs, Lsrt = idx[order], L[order]
    st = np.searchsorted(idxs, np.arange(nb), "left")
    en = np.searchsorted(idxs, np.arange(nb), "right")
    for b in range(nb):
        count[b] = en[b] - st[b]
        if count[b]:
            v = Lsrt[st[b]:en[b]]
            mean[b] = v.mean()
            p99[b], p999[b] = np.percentile(v, [99, 99.9])

    fig, (a1, a2) = plt.subplots(2, 1, figsize=(7, 6), sharex=True)
    fig.suptitle(args.title)
    a1.plot(centers, mean, color="tab:blue", label="Mean")
    a1.set_ylabel("Latency (µs)")
    a1.legend(loc="upper left"); a1.grid(True, alpha=0.3)

    a2.plot(centers, p99, color="tab:blue", label="99th percentile")
    a2.plot(centers, p999, color="tab:orange", label="99.9th percentile")
    a2.set_ylabel("Latency (µs)"); a2.set_xlabel("Distance from AP (m)")
    a2.legend(loc="center right"); a2.grid(True, alpha=0.3)

    fig.tight_layout(rect=[0, 0, 1, 0.95])
    fig.savefig(args.outfile, dpi=130)
    plt.close(fig)
    print("wrote", args.outfile)

    if args.csv:
        with open(args.csv, "w") as fh:
            fh.write("distance_m,mean_us,p99_us,p999_us,count\n")
            for i, c in enumerate(centers):
                fh.write(f"{c:.4g},{mean[i]:.6g},{p99[i]:.6g},{p999[i]:.6g},{count[i]}\n")
        print("wrote", args.csv)


if __name__ == "__main__":
    main()
