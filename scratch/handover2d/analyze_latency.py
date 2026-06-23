#!/usr/bin/env python3
"""
analyze_latency.py
==================

Bin a handover STA log by STA position and report per-bin latency statistics
(mean, P99, P99.9), separately for the two travel directions, optionally
overlaying the two scenarios (passive same-channel vs active different-channel).

Designed for very large logs (10000 round trips ~= 50M packets, tens of GB):
  * the file is parsed line-by-line (never fully loaded),
  * samples are stored in compact float32 numpy buffers (not Python tuples),
  * binning uses a single sort instead of one full pass per bin.

INPUT  : the STA log JSON produced by handover.cc (--staLogFile output).
DIRECTION : leg = floor(tx_time / tripTime); even leg = AP1->AP2 (outbound,
            x increasing), odd leg = AP2->AP1 (return, x decreasing).
POSITION  : STA x at the packet's first transmission (transmissions[0].position[0]).
LATENCY   : packet-level end-to-end latency info["latency"] (creation -> ACK/drop),
            in ms. All packets with >=1 transmission are included. Packets that
            expired in the MAC queue with no transmission are counted, not binned.

USAGE
-----
    python analyze_latency.py \
        --passive handover_sta_passive.json \
        --active  handover_sta_active.json \
        --binwidths 1,2.5,5 \
        --outdir results

Either --passive or --active (or both) may be given.

Tip: `pip install orjson` makes parsing several times faster; it is used
automatically if present.
"""

import argparse
import json
import math
import os

import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# optional fast JSON parser
try:
    import orjson
    def _loads(s):
        return orjson.loads(s)
except ImportError:
    def _loads(s):
        return json.loads(s)


# --------------------------------------------------------------------------- #
# Growable float32 buffer (amortised O(1) append, compact memory)
# --------------------------------------------------------------------------- #
class Buffer:
    def __init__(self, dtype=np.float32, cap=1 << 20):
        self._a = np.empty(cap, dtype=dtype)
        self._n = 0

    def append(self, v):
        if self._n == self._a.size:
            self._a = np.resize(self._a, self._a.size * 2)
        self._a[self._n] = v
        self._n += 1

    def array(self):
        return self._a[:self._n]


# --------------------------------------------------------------------------- #
# Streaming log reader
# --------------------------------------------------------------------------- #
def iter_records(path):
    """Yield each JSON object in the STA log, one at a time."""
    buf = ""
    with open(path, "r") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line == "[" or line == "]":
                continue
            if line.startswith("["):
                line = line[1:].strip()
            if line.endswith("]"):
                line = line[:-1].strip()
            if line.endswith(","):
                line = line[:-1].strip()
            if not line:
                continue
            candidate = (buf + line) if buf else line
            try:
                obj = _loads(candidate)
                buf = ""
                yield obj
            except (json.JSONDecodeError, ValueError):
                buf = candidate + " "
        if buf.strip():
            try:
                yield _loads(buf)
            except (json.JSONDecodeError, ValueError):
                pass


# --------------------------------------------------------------------------- #
# Direction codes
# --------------------------------------------------------------------------- #
FWD = 0   # AP1 -> AP2 (outbound)
BWD = 1   # AP2 -> AP1 (return)
DIRECTIONS = [(FWD, "AP1_to_AP2"), (BWD, "AP2_to_AP1")]
DIR_LABEL = {"AP1_to_AP2": "AP1 → AP2 (outbound, x increasing)",
             "AP2_to_AP1": "AP2 → AP1 (return, x decreasing)"}


def collect(path):
    """Stream a log into compact arrays.

    Returns (config, xs[float32], lats[float32], dirs[int8], info)."""
    it = iter_records(path)
    config = next(it)                      # first record = config header
    trip_time = float(config.get("tripTime", 75.0))

    xs, ls, ds = Buffer(np.float32), Buffer(np.float32), Buffer(np.int8)
    n_total = n_no_tx = 0

    for rec in it:
        if "elapsed_seconds" in rec:       # footer
            continue
        if "latency" not in rec:
            continue
        n_total += 1

        txs = rec.get("transmissions") or []
        if not txs:
            n_no_tx += 1
            continue
        first_tx = txs[0]
        pos = first_tx.get("position")
        tx_time_ns = first_tx.get("tx_time")
        if pos is None or tx_time_ns is None:
            n_no_tx += 1
            continue

        t = float(tx_time_ns) / 1e9
        leg = int(t // trip_time) if trip_time > 0 else 0
        xs.append(float(pos[0]))
        ls.append(float(rec["latency"]) / 1e6)      # ns -> ms
        ds.append(FWD if (leg % 2 == 0) else BWD)

    return (config, xs.array(), ls.array(), ds.array(),
            dict(n_total=n_total, n_no_tx=n_no_tx))


def collect_many(paths, nproc=1):
    """Collect one or several shard logs into a single pooled set of arrays.

    With nproc > 1 the shard files are parsed in parallel processes."""
    if len(paths) == 1:
        return collect(paths[0])

    results = {}
    if nproc and nproc > 1:
        import concurrent.futures as cf
        with cf.ProcessPoolExecutor(max_workers=min(nproc, len(paths))) as ex:
            fut = {ex.submit(collect, p): p for p in paths}
            for f in cf.as_completed(fut):
                results[fut[f]] = f.result()
    else:
        for p in paths:
            results[p] = collect(p)

    cfg = None
    xs_parts, ls_parts, ds_parts = [], [], []
    n_total = n_no_tx = 0
    for p in paths:                                   # keep deterministic order
        c, x, l, d, info = results[p]
        if cfg is None:
            cfg = c
        xs_parts.append(x); ls_parts.append(l); ds_parts.append(d)
        n_total += info["n_total"]; n_no_tx += info["n_no_tx"]
    return (cfg,
            np.concatenate(xs_parts), np.concatenate(ls_parts),
            np.concatenate(ds_parts),
            dict(n_total=n_total, n_no_tx=n_no_tx))


# --------------------------------------------------------------------------- #
# Binning + statistics (single sort, then slice per bin)
# --------------------------------------------------------------------------- #
def bin_stats(xs, ls, bin_width, x_min, x_max):
    """xs, ls are 1-D arrays already filtered to one direction."""
    if xs is None or xs.size == 0:
        return None
    edges = np.arange(x_min, x_max + bin_width, bin_width)
    nbins = len(edges) - 1
    idx = np.floor((xs - x_min) / bin_width).astype(np.int64)
    np.clip(idx, 0, nbins - 1, out=idx)

    order = np.argsort(idx, kind="stable")
    idx_s = idx[order]
    ls_s = ls[order]
    starts = np.searchsorted(idx_s, np.arange(nbins), side="left")
    ends = np.searchsorted(idx_s, np.arange(nbins), side="right")

    centers = edges[:-1] + bin_width / 2.0
    mean = np.full(nbins, np.nan)
    p99 = np.full(nbins, np.nan)
    p999 = np.full(nbins, np.nan)
    count = (ends - starts).astype(int)

    for b in range(nbins):
        if count[b]:
            vals = ls_s[starts[b]:ends[b]]
            mean[b] = vals.mean()
            p99[b], p999[b] = np.percentile(vals, [99, 99.9])

    return dict(centers=centers, mean=mean, p99=p99, p999=p999, count=count)


# --------------------------------------------------------------------------- #
# Plotting
# --------------------------------------------------------------------------- #
METRICS = [("mean", "Mean latency"),
           ("p99", "P99 latency"),
           ("p999", "P99.9 latency")]


def ap_lines(ax, config):
    aps = config.get("apPositions", [])
    xs = [ap["x"] for ap in aps]
    for i, xv in enumerate(xs):
        ax.axvline(xv, color="grey", ls="--", lw=0.8, alpha=0.7)
        ax.annotate(f"AP{i+1}", xy=(xv, 1.0), xycoords=("data", "axes fraction"),
                    ha="center", va="bottom", fontsize=8, color="grey")
    if len(xs) == 2:
        ax.axvline((xs[0] + xs[1]) / 2.0, color="orange", ls=":", lw=0.8, alpha=0.7)


def _draw(ax, key, sp, sa):
    if sp is not None:
        ax.plot(sp["centers"], sp[key], color="tab:blue", marker=".", ms=4, lw=1.3,
                label="passive (same channel)")
    if sa is not None:
        ax.plot(sa["centers"], sa[key], color="tab:red", marker=".", ms=4, lw=1.3,
                label="active (different channels)")


def plot_direction(dname, bin_width, sp, sa, config, outdir):
    fig, axes = plt.subplots(3, 1, figsize=(10, 11), sharex=True)
    fig.suptitle(f"Latency vs STA position  |  {DIR_LABEL[dname]}\n"
                 f"bin width = {bin_width} m", fontsize=12)
    for ax, (key, label) in zip(axes, METRICS):
        _draw(ax, key, sp, sa)
        ap_lines(ax, config)
        ax.set_ylabel(f"{label} (ms)")
        ax.grid(True, alpha=0.3)
        ax.legend(loc="upper right", fontsize=8)
    axes[-1].set_xlabel("STA x position (m)")
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fname = os.path.join(outdir, f"latency_W{bin_width}m_{dname}.png")
    fig.savefig(fname, dpi=130)
    plt.close(fig)
    return fname


def plot_single_metric(dname, bin_width, key, label, sp, sa, config, outdir):
    fig, ax = plt.subplots(figsize=(10, 4.2))
    _draw(ax, key, sp, sa)
    ap_lines(ax, config)
    ax.set_title(f"{label}  |  {DIR_LABEL[dname]}  |  bin {bin_width} m")
    ax.set_xlabel("STA x position (m)")
    ax.set_ylabel(f"{label} (ms)")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper right", fontsize=8)
    fig.tight_layout()
    fname = os.path.join(outdir, f"latency_W{bin_width}m_{dname}_{key}.png")
    fig.savefig(fname, dpi=130)
    plt.close(fig)
    return fname


def write_csv(dname, bin_width, sp, sa, outdir):
    fname = os.path.join(outdir, f"latency_W{bin_width}m_{dname}.csv")
    centers = (sp or sa)["centers"]

    def col(s, k):
        return s[k] if s is not None else np.full(len(centers), np.nan)

    with open(fname, "w") as fh:
        fh.write("x_center_m,"
                 "passive_mean_ms,passive_p99_ms,passive_p999_ms,passive_count,"
                 "active_mean_ms,active_p99_ms,active_p999_ms,active_count\n")
        for i, c in enumerate(centers):
            fh.write(",".join([
                f"{c:.4g}",
                f"{col(sp,'mean')[i]:.6g}", f"{col(sp,'p99')[i]:.6g}",
                f"{col(sp,'p999')[i]:.6g}",
                f"{int(col(sp,'count')[i]) if sp is not None else 0}",
                f"{col(sa,'mean')[i]:.6g}", f"{col(sa,'p99')[i]:.6g}",
                f"{col(sa,'p999')[i]:.6g}",
                f"{int(col(sa,'count')[i]) if sa is not None else 0}",
            ]) + "\n")
    return fname


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--passive", nargs="+", default=None,
                    help="one or more STA logs for the passive/same-channel "
                         "scenario (shards are pooled). Globs work: "
                         "--passive logs/passive_shard*.json")
    ap.add_argument("--active", nargs="+", default=None,
                    help="one or more STA logs for the active/different-channel "
                         "scenario (shards are pooled)")
    ap.add_argument("--binwidths", default="1,2.5,5",
                    help="comma-separated bin widths in metres (default 1,2.5,5)")
    ap.add_argument("--outdir", default="results")
    ap.add_argument("--jobs", type=int, default=1,
                    help="parse shard files in this many parallel processes")
    ap.add_argument("--xmin", type=float, default=None)
    ap.add_argument("--xmax", type=float, default=None)
    ap.add_argument("--no-single", action="store_true",
                    help="skip the per-metric single-panel PNGs")
    args = ap.parse_args()

    if not args.passive and not args.active:
        ap.error("provide at least one of --passive / --active")

    bin_widths = [float(b) for b in args.binwidths.split(",") if b.strip()]
    os.makedirs(args.outdir, exist_ok=True)

    cfg_p = xs_p = ls_p = ds_p = None
    cfg_a = xs_a = ls_a = ds_a = None

    if args.passive:
        print(f"Reading passive log(s): {len(args.passive)} file(s)")
        cfg_p, xs_p, ls_p, ds_p, info_p = collect_many(args.passive, args.jobs)
        print(f"  packets={info_p['n_total']}  no-tx(skipped)={info_p['n_no_tx']}  "
              f"forward={int((ds_p == FWD).sum())}  backward={int((ds_p == BWD).sum())}")

    if args.active:
        print(f"Reading active  log(s): {len(args.active)} file(s)")
        cfg_a, xs_a, ls_a, ds_a, info_a = collect_many(args.active, args.jobs)
        print(f"  packets={info_a['n_total']}  no-tx(skipped)={info_a['n_no_tx']}  "
              f"forward={int((ds_a == FWD).sum())}  backward={int((ds_a == BWD).sum())}")

    config = cfg_p if cfg_p is not None else cfg_a
    x_min = args.xmin if args.xmin is not None else float(
        config.get("staPosStart", {}).get("x", 0.0))
    x_max = args.xmax if args.xmax is not None else float(
        config.get("staPosEnd", {}).get("x", 150.0))
    if x_max < x_min:
        x_min, x_max = x_max, x_min
    print(f"Trajectory x range: [{x_min}, {x_max}] m")

    made = []
    for bw in bin_widths:
        for code, dname in DIRECTIONS:
            sp = sa = None
            if xs_p is not None:
                m = ds_p == code
                sp = bin_stats(xs_p[m], ls_p[m], bw, x_min, x_max)
            if xs_a is not None:
                m = ds_a == code
                sa = bin_stats(xs_a[m], ls_a[m], bw, x_min, x_max)
            if sp is None and sa is None:
                continue
            made.append(plot_direction(dname, bw, sp, sa, config, args.outdir))
            made.append(write_csv(dname, bw, sp, sa, args.outdir))
            if not args.no_single:
                for key, label in METRICS:
                    made.append(plot_single_metric(
                        dname, bw, key, label, sp, sa, config, args.outdir))

    print(f"\nWrote {len(made)} files to {args.outdir}/")
    for f in made:
        print("  " + f)


if __name__ == "__main__":
    main()
