#!/usr/bin/env python3
"""
aggregate_energy.py — 汇总所有拓扑的 energy_unified.csv，出对比表 + 图。

各 *-sim.cc 在 main() 末尾调用
    EnergyModel::WriteCsv("energy_unified.csv", topo, scenario)
会把每次运行追加一行到同一个 CSV。本脚本读它，按 (topology, scenario) 汇总，
打印对比表，并(可选)画两张图：平均功耗(W) 与 网络能效(pJ/每交付比特)。

用法:
  python3 aggregate_energy.py energy_unified.csv
  python3 aggregate_energy.py energy_unified.csv --scenario allreduce --plot
"""
import argparse
import csv

FLOAT_COLS = ("staticW", "avgW", "totalJ", "staticFrac", "perGpuW",
              "pJ_per_delivered_bit", "durSec", "rateGbps",
              "bisectionGbps", "bisecPerGpuGbps", "portsPerGpu", "cablesPerGpu",
              "xcvrPerGpu", "wPerBisecGbps", "tputGbps", "bisecUtil", "gbpsPerW")
INT_COLS = ("nSwitch", "nSwitchPorts", "nHostPorts", "nNic", "nFpga",
            "nOpticalPorts", "nGpu", "linkBits", "deliveredBits")


def load(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            for k in FLOAT_COLS:
                v = r.get(k, "")
                r[k] = float(v) if v not in (None, "") else 0.0
            for k in INT_COLS:
                v = r.get(k, "")
                r[k] = int(float(v)) if v not in (None, "") else 0
            rows.append(r)
    return rows


def fmt_w(w):
    return f"{w/1000:.1f} kW" if w >= 1000 else f"{w:.0f} W"


def table(rows):
    hdr = (f"{'topology':<20}{'scenario':<16}{'sw':>4}{'fpga':>6}"
           f"{'gpu':>6}{'avgPower':>11}{'perGPU(W)':>11}"
           f"{'staticFrac':>11}{'pJ/bit':>10}")
    print(hdr)
    print("-" * len(hdr))
    for r in sorted(rows, key=lambda x: x["avgW"]):
        print(f"{r['topology']:<20}{r['scenario']:<16}"
              f"{r['nSwitch']:>4}{r['nFpga']:>6}{r['nGpu']:>6}"
              f"{fmt_w(r['avgW']):>11}{r['perGpuW']:>11.1f}"
              f"{r['staticFrac']*100:>10.0f}%{r['pJ_per_delivered_bit']:>10.0f}")


def fairness_table(rows):
    """apples-to-apples：每 GPU 配了多少资源 + 性价比。"""
    hdr = (f"{'topology':<20}{'scenario':<16}{'bisec/GPU':>11}"
           f"{'ports/GPU':>10}{'cable/GPU':>10}{'xcvr/GPU':>10}"
           f"{'W/Gbps-bis':>12}{'bisUtil':>9}{'Gbps/W':>9}")
    print("\n=== FAIRNESS (apples-to-apples, per-GPU provisioning) ===")
    print(hdr)
    print("-" * len(hdr))
    # 按"配单位对分带宽的瓦数"排序：越小越省
    for r in sorted(rows, key=lambda x: x["wPerBisecGbps"] or 1e9):
        print(f"{r['topology']:<20}{r['scenario']:<16}"
              f"{r['bisecPerGpuGbps']:>11.1f}{r['portsPerGpu']:>10.1f}"
              f"{r['cablesPerGpu']:>10.1f}{r['xcvrPerGpu']:>10.1f}"
              f"{r['wPerBisecGbps']:>12.3f}{r['bisecUtil']:>9.2f}"
              f"{r['gbpsPerW']:>9.3f}")
    print("\n读法：bisec/GPU 高=每 GPU 跨网带宽多；ports/cable/xcvr/GPU 低=硬件省；")
    print("      W/Gbps-bis 低=配带宽性价比高；Gbps/W 高=能效高。")
    print("      若某拓扑吞吐高但 ports/GPU 也高 → 是'堆 NIC'而非真高效。")


def plot(rows, out_prefix="energy"):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    rows = sorted(rows, key=lambda x: x["avgW"])
    labels = [f"{r['topology']}\n{r['scenario']}" for r in rows]

    for metric, ylabel, fname in [
        ("avgW", "Average network power (W)", f"{out_prefix}_power.png"),
        ("perGpuW", "Network power per GPU (W)", f"{out_prefix}_per_gpu.png"),
        ("pJ_per_delivered_bit", "Energy per delivered bit (pJ/bit)",
         f"{out_prefix}_efficiency.png"),
        ("wPerBisecGbps", "Watts per Gbps-bisection  (provisioning cost)",
         f"{out_prefix}_cost_per_bw.png"),
        ("bisecPerGpuGbps", "Bisection BW per GPU (Gbps)  (higher = better)",
         f"{out_prefix}_bisection_per_gpu.png"),
    ]:
        vals = [r[metric] for r in rows]
        plt.figure(figsize=(max(8, len(rows) * 1.3), 5))
        bars = plt.bar(range(len(rows)), vals)
        plt.xticks(range(len(rows)), labels, rotation=30, ha="right", fontsize=8)
        plt.ylabel(ylabel)
        plt.title(ylabel + "  (lower = better)")
        for b, v in zip(bars, vals):
            lbl = f"{v:.2f}" if v < 10 else f"{v:.0f}"
            plt.text(b.get_x() + b.get_width() / 2, v, lbl,
                     ha="center", va="bottom", fontsize=8)
        plt.tight_layout()
        plt.savefig(fname, dpi=130)
        print(f"  wrote {fname}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="?", default="energy_unified.csv")
    ap.add_argument("--scenario", help="only this scenario")
    ap.add_argument("--plot", action="store_true")
    a = ap.parse_args()

    rows = load(a.csv)
    if a.scenario:
        rows = [r for r in rows if r["scenario"] == a.scenario]
    if not rows:
        print("no rows")
        return
    table(rows)
    fairness_table(rows)
    if a.plot:
        plot(rows)


if __name__ == "__main__":
    main()
