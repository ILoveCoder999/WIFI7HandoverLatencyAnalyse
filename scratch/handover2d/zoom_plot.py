#!/usr/bin/env python3
"""
zoom_plot.py — re-plot the binned-latency CSVs with a capped y-axis (zoom),
without re-parsing the multi-GB logs.  Reads the latency_W*m_*.csv files that
analyze_latency.py already produced and redraws them with --ymax.

USAGE
-----
    python zoom_plot.py --indir results/handover_parallel/analysis --ymax 10
    python zoom_plot.py --indir results/handover_parallel/analysis_active --ymax 15

    # To set different ymax for each subplot:
    python zoom_plot.py --indir ... --ymax-mean 1 --ymax-p99 20 --ymax-p999 30
    # (--ymax is now optional, default 10)
"""
import argparse, csv, glob, os, re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

METRICS = [("mean", "Mean latency"), ("p99", "P99 latency"), ("p999", "P99.9 latency")]
DIR_LABEL = {"AP1_to_AP2": "AP1 → AP2 (outbound)",
             "AP2_to_AP1": "AP2 → AP1 (return)"}


def load_csv(path):
    rows = list(csv.DictReader(open(path)))
    cols = {k: [] for k in rows[0].keys()} if rows else {}
    for r in rows:
        for k, v in r.items():
            try:
                cols[k].append(float(v))
            except (ValueError, TypeError):
                cols[k].append(float("nan"))
    return cols


def has_data(cols, prefix):
    key = prefix + "_count"
    return key in cols and any(c and c == c for c in cols[key])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--indir", required=True, help="dir with latency_W*m_*.csv files")
    # ---------- 关键修改：--ymax 不再是 required，改为默认值 ----------
    ap.add_argument("--ymax", type=float, default=10.0,
                    help="y-axis cap (ms) — fallback for all")
    ap.add_argument("--ymin", type=float, default=0.0)
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--aps", default="60,90", help="AP x positions for markers")
    ap.add_argument("--ymax-mean", type=float, default=None,
                    help="y-axis cap for mean subplot (ms), overrides --ymax")
    ap.add_argument("--ymax-p99", type=float, default=None,
                    help="y-axis cap for P99 subplot (ms), overrides --ymax")
    ap.add_argument("--ymax-p999", type=float, default=None,
                    help="y-axis cap for P99.9 subplot (ms), overrides --ymax")
    args = ap.parse_args()

    outdir = args.outdir or (args.indir.rstrip("/") + f"_zoom{args.ymax:g}")
    os.makedirs(outdir, exist_ok=True)
    aps = [float(x) for x in args.aps.split(",") if x.strip()]

    ymax_list = [
        args.ymax_mean if args.ymax_mean is not None else args.ymax,
        args.ymax_p99  if args.ymax_p99  is not None else args.ymax,
        args.ymax_p999 if args.ymax_p999 is not None else args.ymax,
    ]

    csvs = sorted(glob.glob(os.path.join(args.indir, "latency_W*m_*.csv")))
    if not csvs:
        raise SystemExit(f"no latency_W*m_*.csv in {args.indir}")

    made = 0
    for path in csvs:
        name = os.path.basename(path)[:-4]
        m = re.match(r"latency_W([\d.]+)m_(AP\d_to_AP\d)", name)
        bw = m.group(1) if m else "?"
        direction = m.group(2) if m else name
        cols = load_csv(path)
        x = cols["x_center_m"]

        fig, axes = plt.subplots(3, 1, figsize=(10, 11), sharex=True)
        fig.suptitle(f"Latency vs STA position (zoom ≤ {args.ymax:g} ms)  |  "
                     f"{DIR_LABEL.get(direction, direction)}  |  bin {bw} m",
                     fontsize=12)
        for ax_idx, (ax, (key, label)) in enumerate(zip(axes, METRICS)):
            if has_data(cols, "passive"):
                ax.plot(x, cols[f"passive_{key}_ms"], color="tab:blue",
                        marker=".", ms=4, lw=1.3, label="passive (same channel)")
            if has_data(cols, "active"):
                ax.plot(x, cols[f"active_{key}_ms"], color="tab:red",
                        marker=".", ms=4, lw=1.3, label="active (different channels)")
            for i, xv in enumerate(aps):
                ax.axvline(xv, color="grey", ls="--", lw=0.8, alpha=0.7)
                ax.annotate(f"AP{i+1}", xy=(xv, 1.0), xycoords=("data", "axes fraction"),
                            ha="center", va="bottom", fontsize=8, color="grey")
            if len(aps) == 2:
                ax.axvline(sum(aps) / 2, color="orange", ls=":", lw=0.8, alpha=0.7)
            ax.set_ylim(args.ymin, ymax_list[ax_idx])
            ax.set_ylabel(f"{label} (ms)")
            ax.grid(True, alpha=0.3)
            ax.legend(loc="upper right", fontsize=8)
        axes[-1].set_xlabel("STA x position (m)")
        fig.tight_layout(rect=[0, 0, 1, 0.96])
        out = os.path.join(outdir, f"{name}_zoom{args.ymax:g}.png")
        fig.savefig(out, dpi=130)
        plt.close(fig)
        made += 1

    print(f"wrote {made} zoomed figures to {outdir}/")


if __name__ == "__main__":
    main()