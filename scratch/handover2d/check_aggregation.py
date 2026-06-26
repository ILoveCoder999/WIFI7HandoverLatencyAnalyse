#!/usr/bin/env python3
"""
check_aggregation.py — quantify how often A-MPDU frame aggregation occurs and
where (by STA position), to confirm that aggregation is effectively inactive
during normal data flow.

Two MPDUs that are aggregated into one A-MPDU share the same PHY transmit time
(tx_time), so an aggregated PPDU is detected as a tx_time carrying more than one
logged packet.

USAGE
    python check_aggregation.py results/handover_parallel/sta_active_shard*.json
"""
import json, sys
from collections import defaultdict, Counter

try:
    import orjson; _loads = orjson.loads
except ImportError:
    _loads = json.loads


def scan(path):
    bytx = defaultdict(list)            # tx_time -> [STA x positions]
    with open(path) as fh:
        first = True
        for raw in fh:
            s = raw.strip().rstrip(",")
            if not s or s in "[]":
                continue
            if first:                   # skip the config header line
                first = False
                continue
            try:
                r = _loads(s)
            except (json.JSONDecodeError, ValueError):
                continue
            if isinstance(r, dict) and r.get("transmissions"):
                t = r["transmissions"][0]
                bytx[t["tx_time"]].append(t["position"][0])
    ppdu = len(bytx)
    agg = [xs for xs in bytx.values() if len(xs) > 1]
    return ppdu, agg


def main():
    files = sys.argv[1:]
    if not files:
        sys.exit("usage: python check_aggregation.py <sta_log.json> [more ...]")

    print(f"{'shard':<40} {'PPDUs':>10} {'aggregated':>11} {'aggregated %':>13}")
    print("-" * 78)
    tot_ppdu = 0
    tot_agg = 0
    all_positions = []
    for f in files:
        ppdu, agg = scan(f)
        nagg = len(agg)
        tot_ppdu += ppdu
        tot_agg += nagg
        all_positions += [x for g in agg for x in g]
        pct = 100 * nagg / ppdu if ppdu else 0
        print(f"{f.split('/')[-1]:<40} {ppdu:>10,} {nagg:>11,} {pct:>12.3f}%")
    print("-" * 78)
    pct = 100 * tot_agg / tot_ppdu if tot_ppdu else 0
    print(f"{'TOTAL':<40} {tot_ppdu:>10,} {tot_agg:>11,} {pct:>12.3f}%")

    print("\nPositions where aggregation occurs (STA x, 10 m bins):")
    if all_positions:
        c = Counter(int(x // 10) * 10 for x in all_positions)
        for k in sorted(c):
            print(f"  x = {k:>3}-{k+10:<3} m : {c[k]:,} aggregated packets")
    else:
        print("  (none)")


if __name__ == "__main__":
    main()
