
#!/usr/bin/env python3
"""
analyze_phases.py — 按四阶段拆解每次 handover 的中断时间

Phase ①: 链路失效检测  = De-assoc时刻 − 旧AP最后一次收到的 beacon 时刻
Phase ②: 信道扫描      = 总中断 − Phase① − Phase③
Phase ③: 关联握手      = Re-assoc时刻 − 新AP第一次 BeaconInfo 时刻 (扫描到新AP→完成关联)
Phase ④: 网络层恢复    = 第一个成功传输的包的 tx_time − Re-assoc时刻
"""
import json, sys, math
from pathlib import Path

def ns_to_ms(ns): return ns / 1_000_000.0
def ns_to_s(ns):  return ns / 1_000_000_000.0

AP_NAMES = {"00:00:00:00:00:02": "AP1", "00:00:00:00:00:03": "AP2"}
def ap_name(mac): return AP_NAMES.get(mac, mac)

sta_path   = Path(sys.argv[1]) if len(sys.argv)>1 else Path("handover_sta_log.json")
assoc_path = Path(sys.argv[2]) if len(sys.argv)>2 else Path("handover_assoc_log.json")

with open(sta_path)   as f: sta_raw   = json.load(f)
with open(assoc_path) as f: assoc_raw = json.load(f)

packets = [p for p in sta_raw if "seq" in p]

# 所有事件按时间排序
all_events = sorted([e for e in assoc_raw if "msg" in e and "tx_time" in e],
                    key=lambda e: e["tx_time"])

# ── 找两次真实 handover（from_ap != to_ap）────────────────────────────────────
assoc_only = [e for e in all_events if e["msg"] in ("Association","De-association")]
handovers = []
for i, ev in enumerate(assoc_only):
    if ev["msg"] == "De-association":
        from_mac = ev["ap_info"] if isinstance(ev["ap_info"],str) else ev["ap_info"]["apAddr"]
        deassoc_t = ev["tx_time"]
        # 下一次 Association
        to_mac = None; reassoc_t = None
        for j in range(i+1, len(assoc_only)):
            if assoc_only[j]["msg"] == "Association":
                ai = assoc_only[j]["ap_info"]
                to_mac = ai if isinstance(ai,str) else ai["apAddr"]
                reassoc_t = assoc_only[j]["tx_time"]
                break
        if reassoc_t and from_mac != to_mac:
            handovers.append({"from_ap":from_mac,"to_ap":to_mac,
                               "deassoc_t":deassoc_t,"reassoc_t":reassoc_t})

print("="*70)
print("HANDOVER PHASE BREAKDOWN")
print("="*70)

for ho in handovers:
    from_ap = ho["from_ap"]; to_ap = ho["to_ap"]
    deassoc_t = ho["deassoc_t"]; reassoc_t = ho["reassoc_t"]
    total_ms = ns_to_ms(reassoc_t - deassoc_t)

    label = f"{ap_name(from_ap)}→{ap_name(to_ap)}"
    print(f"\n{'─'*70}")
    print(f"  {label}   de-assoc={ns_to_s(deassoc_t):.4f}s  re-assoc={ns_to_s(reassoc_t):.4f}s")
    print(f"  Total interruption: {total_ms:.3f} ms")

    # ── Phase ①: 旧AP最后一个beacon → de-assoc ──────────────────────────────
    last_old_beacon = None
    for e in all_events:
        if e["tx_time"] >= deassoc_t: break
        if e["msg"] == "BeaconInfo":
            ai = e["ap_info"]
            ap = ai.get("apAddr","") if isinstance(ai,dict) else ai
            if ap == from_ap:
                last_old_beacon = e["tx_time"]

    phase1_ms = ns_to_ms(deassoc_t - last_old_beacon) if last_old_beacon else float("nan")

    # ── Phase ③: 新AP第一个beacon（扫描期间）→ re-assoc ─────────────────────
    # 找 de-assoc 之后、re-assoc 之前，新AP第一次出现的 BeaconInfo
    first_new_beacon = None
    for e in all_events:
        if e["tx_time"] <= deassoc_t: continue
        if e["tx_time"] >= reassoc_t: break
        if e["msg"] == "BeaconInfo":
            ai = e["ap_info"]
            ap = ai.get("apAddr","") if isinstance(ai,dict) else ai
            if ap == to_ap:
                first_new_beacon = e["tx_time"]
                break

    phase3_ms = ns_to_ms(reassoc_t - first_new_beacon) if first_new_beacon else float("nan")

    # ── Phase ②: de-assoc → 新AP第一次beacon ────────────────────────────────
    if first_new_beacon:
        phase2_ms = ns_to_ms(first_new_beacon - deassoc_t)
    else:
        phase2_ms = float("nan")

    # ── Phase ④: re-assoc → 第一个成功acked包的tx_time ──────────────────────
    first_tx_after = None
    for p in sorted(packets, key=lambda x: x.get("transmissions",[{}])[0].get("tx_time",0)
                    if x.get("transmissions") else 0):
        if not p.get("acked"): continue
        txs = p.get("transmissions",[])
        if not txs: continue
        tx_ap = txs[0].get("assoc_ap","")
        tx_t  = txs[0].get("tx_time", 0)
        if tx_t > reassoc_t and tx_ap == to_ap:
            first_tx_after = tx_t
            break

    phase4_ms = ns_to_ms(first_tx_after - reassoc_t) if first_tx_after else float("nan")

    print(f"\n  Phase ①  Link failure detection : {phase1_ms:8.3f} ms"
          f"  (last {ap_name(from_ap)} beacon → de-assoc)")
    print(f"  Phase ②  Channel scan            : {phase2_ms:8.3f} ms"
          f"  (de-assoc → first {ap_name(to_ap)} beacon seen)")
    print(f"  Phase ③  Association handshake   : {phase3_ms:8.3f} ms"
          f"  (first {ap_name(to_ap)} beacon → re-assoc complete)")
    print(f"  Phase ④  Network-layer recovery  : {phase4_ms:8.3f} ms"
          f"  (re-assoc → first acked packet TX)")
    print(f"  {'─'*40}")
    parts = [phase1_ms, phase2_ms, phase3_ms, phase4_ms]
    accounted = sum(p for p in parts if not math.isnan(p))
    print(f"  Sum of phases                    : {accounted:8.3f} ms")
    print(f"  Total measured interruption      : {total_ms:8.3f} ms")

print(f"\n{'='*70}")
print("SUMMARY TABLE")
print("="*70)
print(f"  {'Event':<16} {'①detect':>10} {'②scan':>10} {'③assoc':>10} {'④recovery':>11} {'Total':>10}")
print(f"  {'-'*16} {'-'*10} {'-'*10} {'-'*10} {'-'*11} {'-'*10}")
for ho in handovers:
    from_ap=ho["from_ap"]; to_ap=ho["to_ap"]
    deassoc_t=ho["deassoc_t"]; reassoc_t=ho["reassoc_t"]
    total_ms=ns_to_ms(reassoc_t-deassoc_t)
    label=f"{ap_name(from_ap)}→{ap_name(to_ap)}"

    last_old=next((e["tx_time"] for e in reversed(all_events)
                   if e["tx_time"]<deassoc_t and e["msg"]=="BeaconInfo"
                   and (e["ap_info"].get("apAddr","") if isinstance(e["ap_info"],dict) else e["ap_info"])==from_ap),None)
    p1=ns_to_ms(deassoc_t-last_old) if last_old else float("nan")

    fnb=next((e["tx_time"] for e in all_events
              if e["tx_time"]>deassoc_t and e["tx_time"]<reassoc_t
              and e["msg"]=="BeaconInfo"
              and (e["ap_info"].get("apAddr","") if isinstance(e["ap_info"],dict) else e["ap_info"])==to_ap),None)
    p2=ns_to_ms(fnb-deassoc_t) if fnb else float("nan")
    p3=ns_to_ms(reassoc_t-fnb) if fnb else float("nan")

    fta=next((txs[0]["tx_time"] for p in sorted(packets,
              key=lambda x:(x.get("transmissions")or[{}])[0].get("tx_time",0))
              if p.get("acked") and p.get("transmissions")
              and (txs:=p["transmissions"]) and txs[0].get("tx_time",0)>reassoc_t
              and txs[0].get("assoc_ap","")==to_ap),None)
    p4=ns_to_ms(fta-reassoc_t) if fta else float("nan")

    def fmt(v): return f"{v:10.3f}" if not math.isnan(v) else f"{'N/A':>10}"
    print(f"  {label:<16} {fmt(p1)} {fmt(p2)} {fmt(p3)} {fmt(p4)} {total_ms:>10.3f}")

print("\nDone.")
