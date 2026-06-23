#!/usr/bin/env python3
"""
analyze_pcap_phases.py  (v4 -- Phase 6 removed; flush-frame + loss classification)
==================================================================================
Parse a Wireshark plain-text packet export and break each Wi-Fi handover
into phases (1-5), including ADDBA sub-phase.

CHANGES vs v3:
  * Phase 6 (MAC-flush burst DURATION) statistics REMOVED.
    Reason: it measured the first-to-last timestamp span of the flush burst,
    which is always ~0 in ns-3 because the surviving backlog is sent as a
    single aggregated A-MPDU (one timestamp).  It could not distinguish
    "all expired" (0 frames) from "flushed in one A-MPDU" (N frames) -- both
    gave 0 ms.  We now report the FLUSH FRAME COUNT and a queued/lost estimate.
  * NEW optional --sta-log <handover_sta_log.json>:
    classifies every never-transmitted (dropped) data frame into
      - handover loss      (generated near a handover)
      - coverage-edge loss (STA far from nearest AP -> link too weak)
      - other
    by the STA position at the frame's generation time.

Input (pcap):   Wireshark -> File -> Export Packet Dissections -> As Plain Text
Input (sta-log, optional): the ns-3 per-MPDU JSON log (handover_sta_log.json)

Usage:
    python3 analyze_pcap_phases.py sameactive.txt --mode active
    python3 analyze_pcap_phases.py sameactive.txt --sta-log handover_sta_log.json
"""

import re
import sys
import json
import argparse

# ---------------------------------------------------------------------------
# CLI arguments
# ---------------------------------------------------------------------------

DEFAULT_STA   = "00:00:00_00:00:01"
DEFAULT_AP1   = "00:00:00_00:00:02"
DEFAULT_AP2   = "00:00:00_00:00:03"
DEFAULT_FILES = ["sameactive.txt", "samepassive2.txt",
                 "differentactive2.txt", "differentpassive2.txt"]

parser = argparse.ArgumentParser()
parser.add_argument("pcap_files", nargs="*", default=DEFAULT_FILES)
parser.add_argument("--sta",              default=DEFAULT_STA)
parser.add_argument("--ap1",              default=DEFAULT_AP1)
parser.add_argument("--ap2",              default=DEFAULT_AP2)
parser.add_argument("--trip-time",        type=float, default=75.0,
                    help="One-way trip time (s)")
parser.add_argument("--pos-start",        type=float, default=0.0,
                    help="STA start x-coordinate (m)")
parser.add_argument("--pos-end",          type=float, default=150.0,
                    help="STA end x-coordinate (m)")
parser.add_argument("--packet-interval",  type=float, default=0.030,
                    help="App-layer packet interval (s)")
parser.add_argument("--mode",             choices=["auto","active","passive"],
                    default="auto")
parser.add_argument("--controller-delay", type=float, default=2.0,
                    help="ForceLearn controller push delay (ms), used as Phase-5a fixed value")
# --- NEW: STA-log loss classification ---
parser.add_argument("--sta-log",          default=None,
                    help="ns-3 per-MPDU JSON log (handover_sta_log.json) for loss classification")
parser.add_argument("--client-start",     type=float, default=1.0,
                    help="App client start time (s); frame gen time = client_start + seq*interval")
parser.add_argument("--edge-dist",        type=float, default=45.0,
                    help="Distance (m) to nearest AP above which a loss is 'coverage-edge'. "
                         "Default 45 m: empirically the link sustains TX up to ~50 m in this "
                         "scenario (LogDistance, 16 dBm); tune to your propagation model.")
parser.add_argument("--ho-window",        type=float, default=1.5,
                    help="A loss within +/- this many seconds of a handover is 'handover loss'")
args = parser.parse_args()

STA    = args.sta
AP1    = args.ap1
AP2    = args.ap2
ALL_APS = {AP1, AP2}
AP_NAME = {AP1: "AP1", AP2: "AP2"}

_TRIP      = args.trip_time
_START     = args.pos_start
_END       = args.pos_end
_SPEED     = (_END - _START) / _TRIP       # m/s
_PKT_INTV  = args.packet_interval
_BURST_THR = _PKT_INTV / 6                 # inter-packet gap threshold

def sta_pos(t_s):
    """Return STA x-position (m) at simulation time t_s (s)."""
    cycle = 2 * _TRIP
    t_mod = t_s % cycle
    if t_mod <= _TRIP:
        return _START + _SPEED * t_mod
    else:
        return _END - _SPEED * (t_mod - _TRIP)

def apn(mac): return AP_NAME.get(mac, mac)

# ---------------------------------------------------------------------------
# Frame parsing
# ---------------------------------------------------------------------------

FRAME_RE = re.compile(
    r'^\s*(\d+)\s+(\d+\.\d+)\s+(\S+)\s+(\S+)\s+\S+\s+\d+\s+(.+)$'
)
CTRL_RE = re.compile(
    r'^\s*(\d+)\s+(\d+\.\d+)\s{16,}(\S+)\s+\S+\s+\d+\s+(.+)$'
)

# ---------------------------------------------------------------------------
# Direct .pcap parsing (scapy) -- avoids the manual Wireshark text export
# ---------------------------------------------------------------------------

def _wsfmt(mac):
    """Convert a colon MAC (scapy) to Wireshark's 'oui_nic' form so it matches
    the default --sta/--ap1/--ap2 values (e.g. 00:00:00_00:00:01)."""
    if not mac:
        return ""
    p = mac.lower().split(':')
    if len(p) == 6:
        return f"{p[0]}:{p[1]}:{p[2]}_{p[3]}:{p[4]}:{p[5]}"
    return mac.lower()

def _dot11_info(typ, sub):
    """Map 802.11 (type, subtype) to an info string carrying the keywords the
    frame-type predicates look for."""
    if typ == 0:   # management
        return {0: 'Association Request', 1: 'Association Response',
                4: 'Probe Request',       5: 'Probe Response',
                8: 'Beacon frame',        13: 'Action'}.get(sub, f'Mgmt sub{sub}')
    if typ == 1:   # control
        return {8: 'Block Ack Req', 9: 'Block Ack', 11: 'RTS',
                12: 'CTS', 13: 'Acknowledgement'}.get(sub, f'Ctrl sub{sub}')
    if typ == 2:   # data
        # data-carrying subtypes: 0-3 (legacy Data), 8-11 (QoS Data variants).
        # Null/QoS-Null (4-7, 12-15) carry no payload.
        if sub in (0, 1, 2, 3, 8, 9, 10, 11):
            return 'QoS Data'
        return f'Data Null sub{sub}'
    return f'type{typ} sub{sub}'

def _macstr(b):
    return ':'.join('%02x' % x for x in b)

def parse_pcap(path):
    """Read a classic .pcap (ns-3 radiotap 802.11) by parsing the 802.11 frame
    control + addresses directly from raw bytes -- robust to 802.11be/EHT frames
    that scapy's high-level dissector fails on.  Returns (time, src, dst, info, no)
    tuples, the same shape the text parser produces."""
    import struct
    frames = []
    no = 0
    with open(path, "rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            return frames
        magic = gh[:4]
        if   magic == b'\xd4\xc3\xb2\xa1': endian, div = '<', 1e6   # LE, microsec
        elif magic == b'\xa1\xb2\xc3\xd4': endian, div = '>', 1e6   # BE, microsec
        elif magic == b'\x4d\x3c\xb2\xa1': endian, div = '<', 1e9   # LE, nanosec
        elif magic == b'\xa1\xb2\x3c\x4d': endian, div = '>', 1e9   # BE, nanosec
        else:                              endian, div = '<', 1e6
        linktype = struct.unpack(endian + 'I', gh[20:24])[0]

        while True:
            rh = f.read(16)
            if len(rh) < 16:
                break
            ts_sec, ts_frac, incl, _orig = struct.unpack(endian + 'IIII', rh)
            data = f.read(incl)
            if len(data) < incl or len(data) < 4:
                if len(data) < incl:
                    break
                continue
            t = ts_sec + ts_frac / div

            # strip link-layer header to reach the 802.11 MAC header
            if linktype == 127:            # IEEE802_11_RADIOTAP
                rtlen = data[2] | (data[3] << 8)
            elif linktype == 105:          # IEEE802_11 (no radiotap)
                rtlen = 0
            else:                          # assume radiotap
                rtlen = data[2] | (data[3] << 8)
            mac = data[rtlen:]
            if len(mac) < 10:
                continue

            no += 1
            fc0   = mac[0]
            ftype = (fc0 >> 2) & 0x3
            fsub  = (fc0 >> 4) & 0xf
            dst   = _wsfmt(_macstr(mac[4:10]))            # addr1 = RA/DA
            # ACK (sub 13) and CTS (sub 12) control frames carry only addr1
            has_a2 = not (ftype == 1 and fsub in (12, 13))
            src   = _wsfmt(_macstr(mac[10:16])) if (has_a2 and len(mac) >= 16) else ""
            info  = _dot11_info(ftype, fsub)
            frames.append((t, src, dst, info, no))
    return frames

# ---------------------------------------------------------------------------
# Frame-type predicates
# ---------------------------------------------------------------------------

def is_beacon(src, info, ap):
    return src == ap and 'Beacon frame' in info

def is_probe_req(src, info):
    return src == STA and 'Probe Request' in info

def is_probe_resp(src, dst, info, ap):
    return src == ap and dst == STA and 'Probe Response' in info

def is_assoc_req(src, dst, info, ap):
    return src == STA and dst == ap and 'Association Request' in info

def is_assoc_resp(src, dst, info, ap):
    return src == ap and dst == STA and 'Association Response' in info

def is_data(info):
    # text export: data dissection contains 'iscard';  pcap mode: 'QoS Data'
    return 'iscard' in info or 'QoS Data' in info

def is_action(src, dst, info, ap_src, ap_dst):
    return src == ap_src and dst == ap_dst and 'Action' in info

def is_block_ack(src, dst, info, ap):
    il = info.lower()
    return (src == ap and dst == STA
            and 'block ack' in il
            and 'req' not in il)

def is_ctrl_ack_to_sta(src, dst, info):
    return src == "" and dst == STA and 'Acknowledgement' in info

# ---------------------------------------------------------------------------
# AP-ACK search helpers
# ---------------------------------------------------------------------------

def find_last_ap_ack(frames, before_idx, ap, window=10000):
    for j in range(before_idx - 1, max(before_idx - window, -1), -1):
        t, s, d, i, n = frames[j]
        if is_block_ack(s, d, i, ap) or is_ctrl_ack_to_sta(s, d, i):
            return (t, n)
    return None

def find_first_ap_ack(frames, after_idx, ap, window=5000):
    for j in range(after_idx, min(after_idx + window, len(frames))):
        t, s, d, i, n = frames[j]
        if is_block_ack(s, d, i, ap) or is_ctrl_ack_to_sta(s, d, i):
            return (t, n)
    return None

def find_frame_idx(frames, frame_no, hint=0, span=3000):
    for k in range(hint, min(hint + span, len(frames))):
        if frames[k][4] == frame_no:
            return k
    for k in range(0, len(frames)):
        if frames[k][4] == frame_no:
            return k
    return hint

# ---------------------------------------------------------------------------
# ADDBA / BA-session detection
# ---------------------------------------------------------------------------

def find_addba(frames, j_start, target_ap, search_limit=5000):
    addba_req = addba_resp = None
    found_kind = None
    for k in range(j_start, min(j_start + search_limit, len(frames))):
        tk, sk, dk, ik, nk = frames[k]
        if addba_req is None:
            if is_action(sk, dk, ik, STA, target_ap):
                addba_req = (tk, nk); found_kind = 'addba_sta'; continue
            if is_action(sk, dk, ik, target_ap, STA):
                addba_req = (tk, nk); found_kind = 'addba_ap';  continue
            if is_block_ack(sk, dk, ik, target_ap):
                return (tk, nk), None, 'block_ack'
            if is_data(ik):
                break
        else:
            if found_kind == 'addba_sta' and is_action(sk, dk, ik, target_ap, STA):
                addba_resp = (tk, nk); break
            if found_kind == 'addba_ap'  and is_action(sk, dk, ik, STA, target_ap):
                addba_resp = (tk, nk); break
    return addba_req, addba_resp, found_kind

# ---------------------------------------------------------------------------
# Post-association data collection
# ---------------------------------------------------------------------------

def collect_post_assoc(frames, j_start, assoc_resp_t, stop_fn):
    post = []
    for k in range(j_start, min(j_start + 5000, len(frames))):
        tk, sk, dk, ik, nk = frames[k]
        if stop_fn(sk, dk, ik, tk, assoc_resp_t):
            break
        if is_data(ik):
            post.append((tk, nk))
            if len(post) >= 40:
                break
    return post

def flush_analysis(post_assoc_frames, assoc_resp, ho_anchor, addba_resp):
    """Compute queue-flush FRAME statistics (no duration). Returns a dict.
    ho_anchor = last valid old-AP ACK (data-interrupt start), used for the
    HO-duration / queued-packets estimate (consistent across active & passive)."""
    flush_burst = []
    if post_assoc_frames:
        flush_burst.append(post_assoc_frames[0])
        for idx in range(1, len(post_assoc_frames)):
            gap = post_assoc_frames[idx][0] - post_assoc_frames[idx-1][0]
            if gap < _BURST_THR:
                flush_burst.append(post_assoc_frames[idx])
            else:
                break
        if len(flush_burst) == 1 and len(post_assoc_frames) > 1:
            if post_assoc_frames[1][0] - post_assoc_frames[0][0] >= _BURST_THR:
                flush_burst = []

    n_burst      = len(flush_burst)
    steady_first = post_assoc_frames[n_burst] if len(post_assoc_frames) > n_burst else None
    first_data   = post_assoc_frames[0] if post_assoc_frames else None
    recovery_end = steady_first if steady_first else first_data
    phase5_s     = (recovery_end[0] - assoc_resp[0]) if recovery_end else float('nan')
    wait_s       = (first_data[0]   - assoc_resp[0]) if first_data   else float('nan')
    ho_dur_s     = ((assoc_resp[0] - ho_anchor[0]) if ho_anchor
                    else float('nan'))
    expected_q   = int(ho_dur_s / _PKT_INTV) if ho_dur_s == ho_dur_s else 0
    flushed      = n_burst
    lost_est     = max(0, expected_q - flushed)

    return dict(
        first_data=first_data, flush_burst=flush_burst,
        steady_first=steady_first, recovery_end=recovery_end,
        phase5_s=phase5_s, wait_s=wait_s,
        ho_dur_s=ho_dur_s, expected_in_queue=expected_q,
        flushed_frames=flushed, lost_est=lost_est,
    )

# ---------------------------------------------------------------------------
# Current-AP / last-beacon helpers
# ---------------------------------------------------------------------------

def find_current_ap(frames, i):
    for j in range(i - 1, -1, -1):
        tj, sj, dj, ij, nj = frames[j]
        for ap in ALL_APS:
            if is_assoc_resp(sj, dj, ij, ap):
                return ap
    return None

def find_last_old_beacon(frames, i, current_ap, window=10000):
    for j in range(i - 1, max(i - window, -1), -1):
        tj, sj, dj, ij, nj = frames[j]
        if is_beacon(sj, ij, current_ap):
            return (tj, nj)
    return None

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------

def ms(a, b):
    if a is None or b is None: return float('nan')
    return (b[0] - a[0]) * 1000.0

def fms(v):
    return f"{v:12.6f} ms" if v == v else "            N/A   "

def pos(frame):
    if frame is None: return "N/A"
    return f"{sta_pos(frame[0]):.2f}m"

def pline(name, dur, t0, t1):
    span = (f"x: {pos(t0)} -> {pos(t1)}" if t0 and t1
            else f"x: {pos(t0)} -> N/A" if t0 else "")
    print(f"    {name} : {fms(dur)}   {span}")

LABELS = ["Outbound", "Return", "Event3", "Event4", "Event5"]

# ---------------------------------------------------------------------------
# NEW: STA-log packet-loss classification
# ---------------------------------------------------------------------------

def classify_sta_log(path, handover_times):
    """Classify never-transmitted dropped frames in the ns-3 STA log by STA
    position at frame generation time. Returns a result dict, or None on error.
    Handover losses are attributed to the nearest-in-time handover (per_ho)."""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        print(f"  [STA-LOG] cannot read {path}: {e}")
        return None
    if not isinstance(data, list) or not data:
        print(f"  [STA-LOG] unexpected format: {path}")
        return None

    cfg  = data[0] if isinstance(data[0], dict) else {}
    recs = [r for r in data[1:] if isinstance(r, dict)
            and 'acked' in r and r.get('seq') is not None]

    trip  = cfg.get('tripTime', _TRIP)
    p0    = (cfg.get('staPosStart') or {}).get('x', _START)
    p1    = (cfg.get('staPosEnd')   or {}).get('x', _END)
    speed = (p1 - p0) / trip if trip else 0.0
    intv  = cfg.get('packetInterval', _PKT_INTV)
    aps   = [p.get('x') for p in cfg.get('apPositions', []) if 'x' in p] or [60.0, 90.0]

    def xpos(t):
        cyc = 2 * trip
        tm  = t % cyc
        return p0 + speed * tm if tm <= trip else p1 - speed * (tm - trip)

    never  = [r for r in recs if r.get('acked') is False and not r.get('transmissions')]
    failed = [r for r in recs if r.get('acked') is False and r.get('transmissions')]

    cats   = {'handover': [], 'coverage-edge': [], 'other': []}
    per_ho = [0] * len(handover_times)
    for r in never:
        seq  = r['seq']
        t    = args.client_start + seq * intv
        x    = xpos(t)
        dist = min(abs(x - a) for a in aps)
        near = [abs(t - h) for h in handover_times if abs(t - h) < args.ho_window]
        if near:
            cat = 'handover'
            hi  = min(range(len(handover_times)), key=lambda k: abs(t - handover_times[k]))
            per_ho[hi] += 1
        else:
            cat = 'coverage-edge' if dist > args.edge_dist else 'other'
        cats[cat].append((seq, t, x, dist, str(r.get('addr_1', ''))))

    return dict(path=path, aps=aps, handover_times=handover_times,
                never=len(never), failed=len(failed),
                cats=cats, per_ho=per_ho)


def print_loss_classification(res):
    """Pretty-print the result of classify_sta_log()."""
    if not res:
        return
    cats = res['cats']
    print("\n" + "=" * 72)
    print(f"  PACKET-LOSS CLASSIFICATION  (STA log: {res['path']})")
    print("=" * 72)
    print(f"  Handover times used (s): {[round(h, 2) for h in res['handover_times']] or 'none detected'}")
    print(f"  AP x-positions (m): {res['aps']}   edge-dist threshold: {args.edge_dist:.0f} m")
    print(f"  Never-TX dropped : {res['never']}    Failed-after-TX : {res['failed']}")
    print(f"    - Handover loss      : {len(cats['handover'])}")
    print(f"    - Coverage-edge loss : {len(cats['coverage-edge'])}")
    print(f"    - Other              : {len(cats['other'])}")
    for cat in ('handover', 'coverage-edge', 'other'):
        if not cats[cat]:
            continue
        print(f"  -- {cat} ({len(cats[cat])}) --")
        for seq, t, x, dist, ad in cats[cat]:
            print(f"     seq {seq:<6} t={t:8.2f}s  x={x:7.1f} m  dist={dist:5.1f} m  ->{ad[-2:]}")

# ============================================================================
# Main loop
# ============================================================================

for pcap_file in args.pcap_files:
    print("\n" + "=" * 72)
    print(f"  FILE: {pcap_file}")
    print("=" * 72)

    frames = []
    is_binary = pcap_file.lower().endswith((".pcap", ".pcapng"))
    try:
        if is_binary:
            frames = parse_pcap(pcap_file)
        else:
            with open(pcap_file, encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = FRAME_RE.match(line)
                    if m:
                        no, t, src, dst, info = m.groups()
                        frames.append((float(t), src, dst, info.strip(), int(no)))
                        continue
                    m2 = CTRL_RE.match(line)
                    if m2:
                        no, t, dst, info = m2.groups()
                        frames.append((float(t), "", dst, info.strip(), int(no)))
    except FileNotFoundError:
        print(f"  [ERROR] File not found: {pcap_file}\n"); continue

    print(f"  {len(frames)} frames parsed  ({'pcap/scapy' if is_binary else 'text export'})")

    if args.mode == "auto":
        MODE = "active" if any(is_probe_req(f[1], f[3]) for f in frames) else "passive"
    else:
        MODE = args.mode
    print(f"  Scan mode  : {MODE}")
    print(f"  Controller delay (ForceLearn Phase-5a): {args.controller_delay:.1f} ms\n")

    handovers = []

    # ── Active scan ──────────────────────────────────────────────────────────
    if MODE == "active":
        i = 0
        while i < len(frames):
            t0, src, dst, info, no = frames[i]
            if not is_probe_req(src, info): i += 1; continue

            current_ap = find_current_ap(frames, i)
            if current_ap is None: i += 1; continue

            target_ap       = next(a for a in ALL_APS if a != current_ap)
            last_old_beacon = find_last_old_beacon(frames, i, current_ap)
            last_old_ack    = find_last_ap_ack(frames, i, current_ap)

            probe_resp = assoc_req = assoc_resp = None
            post = []
            for j in range(i + 1, min(i + 600, len(frames))):
                tj, sj, dj, ij, nj = frames[j]
                if probe_resp is None and is_probe_resp(sj, dj, ij, target_ap):
                    probe_resp = (tj, nj)
                if assoc_req is None and is_assoc_req(sj, dj, ij, target_ap):
                    assoc_req = (tj, nj)
                if assoc_resp is None and is_assoc_resp(sj, dj, ij, target_ap):
                    assoc_resp = (tj, nj)
                    post = collect_post_assoc(
                        frames, j + 1, tj,
                        lambda sk, dk, ik, tk, at: is_probe_req(sk, ik) and tk > at + 0.5
                    )
                    break
                if j > i + 2 and is_probe_req(sj, ij): break

            if assoc_resp is not None:
                arsp_idx  = find_frame_idx(frames, assoc_resp[1], i)
                addba_req, addba_resp, addba_kind = find_addba(frames, arsp_idx + 1, target_ap)
                fa        = flush_analysis(post, assoc_resp, last_old_beacon, addba_resp)

                fdat_idx  = find_frame_idx(frames, fa['first_data'][1], arsp_idx) \
                            if fa['first_data'] else arsp_idx + 1
                first_new_ack = find_first_ap_ack(frames, fdat_idx, target_ap)

                handovers.append({
                    "mode": "active",
                    "from_ap": current_ap, "to_ap": target_ap,
                    "last_beacon":   last_old_beacon,
                    "last_old_ack":  last_old_ack,
                    "first_new_ack": first_new_ack,
                    "trigger":       (t0, no),
                    "phase2_end":    probe_resp,
                    "assoc_req":     assoc_req,
                    "assoc_resp":    assoc_resp,
                    "addba_req":     addba_req,
                    "addba_resp":    addba_resp,
                    "addba_kind":    addba_kind,
                    **fa,
                })
                i = arsp_idx + 1
            else:
                i += 1

    # ── Passive scan ─────────────────────────────────────────────────────────
    else:
        i = 0
        while i < len(frames):
            t0, src, dst, info, no = frames[i]
            target_ap = None
            for ap in ALL_APS:
                if is_assoc_req(src, dst, info, ap): target_ap = ap; break
            if target_ap is None: i += 1; continue

            current_ap = find_current_ap(frames, i)
            if current_ap is None or target_ap == current_ap: i += 1; continue

            last_old_beacon = find_last_old_beacon(frames, i, current_ap)
            last_old_ack    = find_last_ap_ack(frames, i, current_ap)

            # First target-AP beacon heard AFTER the data interruption starts
            # (i.e. after the last valid ACK).  On the same channel the new AP's
            # beacons are heard continuously, so anchoring on the last ACK keeps
            # Phase 1 (last ACK -> 1st new beacon) non-negative and meaningful.
            first_new_beacon = None
            anchor_t = (last_old_ack[0] if last_old_ack
                        else last_old_beacon[0] if last_old_beacon else 0.0)
            for j in range(0, i):
                tj, sj, dj, ij, nj = frames[j]
                if tj >= anchor_t and is_beacon(sj, ij, target_ap):
                    first_new_beacon = (tj, nj); break

            assoc_req  = (t0, no)
            assoc_resp = None
            post       = []
            for j in range(i + 1, min(i + 200, len(frames))):
                tj, sj, dj, ij, nj = frames[j]
                if is_assoc_resp(sj, dj, ij, target_ap):
                    assoc_resp = (tj, nj)
                    post = collect_post_assoc(
                        frames, j + 1, tj,
                        lambda sk, dk, ik, tk, at: any(
                            is_assoc_req(sk, dk, ik, a) for a in ALL_APS
                        ) and tk > at + 0.5
                    )
                    break

            if assoc_resp is not None:
                arsp_idx  = find_frame_idx(frames, assoc_resp[1], i)
                addba_req, addba_resp, addba_kind = find_addba(frames, arsp_idx + 1, target_ap)
                fa        = flush_analysis(post, assoc_resp, last_old_beacon, addba_resp)

                fdat_idx  = find_frame_idx(frames, fa['first_data'][1], arsp_idx) \
                            if fa['first_data'] else arsp_idx + 1
                first_new_ack = find_first_ap_ack(frames, fdat_idx, target_ap)

                handovers.append({
                    "mode": "passive",
                    "from_ap": current_ap, "to_ap": target_ap,
                    "last_beacon":      last_old_beacon,
                    "last_old_ack":     last_old_ack,
                    "first_new_ack":    first_new_ack,
                    "trigger":          assoc_req,
                    "first_new_beacon": first_new_beacon,
                    "assoc_req":        assoc_req,
                    "assoc_resp":       assoc_resp,
                    "addba_req":        addba_req,
                    "addba_resp":       addba_resp,
                    "addba_kind":       addba_kind,
                    **fa,
                })
                i = arsp_idx + 1
            else:
                i += 1

    # ── Per-handover detailed output ─────────────────────────────────────────
    print(f"  STA:{STA}  {apn(AP1)}:{AP1}  {apn(AP2)}:{AP2}")
    print(f"  Speed={_SPEED:.2f} m/s  TripTime={_TRIP}s  "
          f"Route=[{_START:.0f}m -> {_END:.0f}m -> {_START:.0f}m]")
    print(f"  Detected {len(handovers)} handover(s)\n")

    ba_labels = {
        'addba_sta': ("ADDBA Req (STA->AP)", "ADDBA Resp (AP->STA)"),
        'addba_ap':  ("ADDBA Req (AP->STA)", "ADDBA Resp (STA->AP)"),
        'block_ack': ("Block Ack (AP->STA, direct)", None),
    }

    for idx, ho in enumerate(handovers):
        label  = LABELS[idx] if idx < len(LABELS) else f"Event{idx+1}"
        route  = f"{apn(ho['from_ap'])} -> {apn(ho['to_ap'])}"
        hmode  = ho["mode"]

        lb         = ho["last_beacon"]
        last_ack   = ho.get("last_old_ack")
        first_nack = ho.get("first_new_ack")
        areq       = ho["assoc_req"]
        arsp       = ho["assoc_resp"]
        fdat       = ho["first_data"]
        rend       = ho["recovery_end"]
        addba_q    = ho.get("addba_req")
        addba_p    = ho.get("addba_resp")
        addba_kind = ho.get("addba_kind")

        print(f"{'─'*72}")
        print(f"  [{label}]  {route}   [{hmode} scan]")
        print(f"{'─'*72}")

        # ── Key frames ───────────────────────────────────────────────────────
        print("  Key frames (time / STA position):")

        if hmode == "active":
            preq = ho["trigger"]
            prsp = ho["phase2_end"]
            if last_ack:
                print(f"    Last {apn(ho['from_ap'])} ACK             "
                      f": t={last_ack[0]:.6f}s  x={pos(last_ack):<8}  Frame#{last_ack[1]}"
                      f"  <- data interrupt start")
            else:
                print(f"    Last {apn(ho['from_ap'])} ACK             : N/A"
                      f"  (no ACK to STA found before Probe Req)")
            print(f"    Probe Request                "
                  f": t={preq[0]:.6f}s  x={pos(preq):<8}  Frame#{preq[1]}")
            if prsp:
                print(f"    Probe Response               "
                      f": t={prsp[0]:.6f}s  x={pos(prsp):<8}  Frame#{prsp[1]}")
        else:
            fnb = ho.get("first_new_beacon")
            if last_ack:
                print(f"    Last {apn(ho['from_ap'])} ACK             "
                      f": t={last_ack[0]:.6f}s  x={pos(last_ack):<8}  Frame#{last_ack[1]}"
                      f"  <- data interrupt start")
            else:
                print(f"    Last {apn(ho['from_ap'])} ACK             : N/A"
                      f"  (no ACK to STA found before handover)")
            if fnb:
                print(f"    First {apn(ho['to_ap'])} Beacon heard    "
                      f": t={fnb[0]:.6f}s  x={pos(fnb):<8}  Frame#{fnb[1]}")

        if areq:
            print(f"    Assoc Request                "
                  f": t={areq[0]:.6f}s  x={pos(areq):<8}  Frame#{areq[1]}")
        if arsp:
            print(f"    Assoc Response               "
                  f": t={arsp[0]:.6f}s  x={pos(arsp):<8}  Frame#{arsp[1]}")
        if addba_q and addba_kind:
            lbl_q, lbl_p = ba_labels.get(addba_kind, ("Session event", None))
            print(f"    {lbl_q:<29}: t={addba_q[0]:.6f}s  x={pos(addba_q):<8}  Frame#{addba_q[1]}")
            if addba_p and lbl_p:
                print(f"    {lbl_p:<29}: t={addba_p[0]:.6f}s  x={pos(addba_p):<8}  Frame#{addba_p[1]}")
        if fdat:
            print(f"    First Data (burst)           "
                  f": t={fdat[0]:.6f}s  x={pos(fdat):<8}  Frame#{fdat[1]}")
        if first_nack:
            print(f"    First {apn(ho['to_ap'])} ACK (burst)        "
                  f": t={first_nack[0]:.6f}s  x={pos(first_nack):<8}  Frame#{first_nack[1]}"
                  f"  <- link restore start")
        else:
            print(f"    First {apn(ho['to_ap'])} ACK (burst)        : N/A")
        if rend and rend != fdat:
            print(f"    Steady-state start           "
                  f": t={rend[0]:.6f}s  x={pos(rend):<8}  Frame#{rend[1]}")

        # ── Phase breakdown (Phases 1-5; Phase 6 removed) ─────────────────────
        print(f"\n  Phase breakdown  (duration | STA x-range):")

        if hmode == "active":
            preq = ho["trigger"]
            prsp = ho["phase2_end"]
            p1   = ms(last_ack, preq)
            p2   = ms(preq,     prsp)
            p3   = ms(prsp,     areq)
            p4   = ms(areq,     arsp)
            p5   = ms(arsp,     fdat)
            p5a_addba = ms(addba_q, addba_p)

            pline("Phase 1  Link Failure Time    (last ACK    -> Probe Req   )", p1, last_ack, preq)
            pline("Phase 2  Channel Probe Time   (Probe Req   -> Probe Resp  )", p2, preq,     prsp)
            pline("Phase 3  Chan Dwell Time      (Probe Resp  -> Assoc Req   )", p3, prsp,     areq)
            pline("Phase 4  Association HandShake(Assoc Req   -> Assoc Resp  )", p4, areq,     arsp)
            pline("Phase 5  Net recovery         (Assoc Resp  -> 1st data    )", p5, arsp,     fdat)
            if addba_q:
                pline("  Phase 5a  ADDBA           (ADDBA Req   -> ADDBA Resp   )", p5a_addba, addba_q, addba_p)

            total_data = ms(last_ack, fdat)
            total_ackd = ms(last_ack, first_nack)
            print(f"    {'─'*68}")
            pline("  Total  Data interruption (last ACK -> 1st new data    )", total_data, last_ack, fdat)
            pline("  Total  Until fully ACKed (last ACK -> burst fully ACKed)", total_ackd, last_ack, first_nack)

        else:  # passive
            fnb  = ho.get("first_new_beacon")
            # Anchor on the last valid ACK (same as active mode), NOT the last beacon
            p1   = ms(last_ack, fnb)
            p3   = ms(fnb,  areq)
            p4   = ms(areq, arsp)
            p5   = ms(arsp, fdat)
            p5a_addba = ms(addba_q, addba_p)

            pline("Phase 1  Link Failure Time     (last ACK        -> 1st new beacon)", p1, last_ack, fnb)
            pline("Phase 3  Chan Dwell Time       (1st new beacon  -> Assoc Req     )", p3, fnb, areq)
            pline("Phase 4  Association HandShake (Assoc Req       -> Assoc Resp    )", p4, areq, arsp)
            pline("Phase 5  Net recovery          (Assoc Resp      -> 1st data      )", p5, arsp, fdat)
            if addba_q:
                pline("  Phase 5a  ADDBA            (ADDBA Req      -> ADDBA Resp   )", p5a_addba, addba_q, addba_p)

            total_data = ms(last_ack, fdat)
            total_ackd = ms(last_ack, first_nack)
            print(f"    {'─'*68}")
            pline("  Total  Data interruption (last ACK -> 1st new data  )", total_data, last_ack, fdat)
            pline("  Total  Until fully ACKed (last ACK -> burst fully ACKed)", total_ackd, last_ack, first_nack)

        # ── Flush / loss statistics (replaces Phase 6 duration) ───────────────
        fb       = ho["flush_burst"]
        p5_s     = ho["phase5_s"]
        ho_dur_s = ho["ho_dur_s"]
        exp_q    = ho["expected_in_queue"]
        flushed  = ho["flushed_frames"]
        lost_est = ho["lost_est"]

        print(f"\n  Flush / loss statistics (frame-count based):")
        print(f"    Phase 5 (Assoc Resp -> steady-state) : {p5_s*1e3:.3f} ms")
        print(f"    Flush frames (air TX)                : {flushed}")
        print(f"    Est. pkts queued during HO           : ~{exp_q}"
              f"  (HO {ho_dur_s*1e3:.0f} ms / {_PKT_INTV*1e3:.0f} ms interval)")
        print(f"    Est. pkts lost/expired (queued-flushed): ~{lost_est}")
        if flushed and fb:
            print(f"    Flush burst start                    : t={fb[0][0]:.6f}s  x={pos(fb[0])}")
        elif flushed == 0:
            note = "(all queued pkts expired, or none queued)"
            print(f"    -> no frames flushed  {note}")
        print()

    # ── classify STA-log losses (if provided): used in summary + detail ───────
    loss_res = None
    if args.sta_log:
        _ho_times = [ho['assoc_resp'][0] for ho in handovers if ho.get('assoc_resp')]
        loss_res = classify_sta_log(args.sta_log, _ho_times)

    # ── Summary table ─────────────────────────────────────────────────────────
    print("=" * 168)
    print(f"  SUMMARY  [{pcap_file}]  (times in ms)  "
          f"ForceLearn controller_delay={args.controller_delay}ms")
    print()
    print(f"  Phase definitions:")
    print(f"    Ph1  Link Failure Time     : last old AP ACK    -> Probe Req        [active]")
    print(f"         Link Failure Time     : last old AP ACK    -> 1st new Beacon   [passive]")
    print(f"    Ph2  Channel Probe Time    : Probe Req          -> Probe Resp       [active only]")
    print(f"    Ph3  Chan Dwell Time       : Probe Resp / 1st new Beacon -> Assoc Req")
    print(f"    Ph4  Association HandShake : Assoc Req          -> Assoc Resp")
    print(f"    Ph5  Net recovery          : Assoc Resp         -> 1st new data frame")
    print(f"    FlushFrames                : # frames actually flushed on air after reassoc")
    print(f"    LostEst                    : ~queued - flushed  (estimated lost/expired)")
    print(f"    HOloss                     : handover-attributed drops from STA log "
          f"({'enabled' if loss_res else 'needs --sta-log'})")
    print("=" * 168)

    hdr = (f"  {'Event':<10} {'Route':<12} {'Mode':<8}"
           f"  {'Ph1 LinkFail':>13} {'Ph2 ChanProbe':>13} {'Ph3 ChanDwell':>13}"
           f"  {'Ph4 AssocHS':>12} {'Ph5 NetRecov':>12}"
           f"  {'FlushFrames':>11} {'LostEst':>8} {'HOloss':>7}"
           f"  {'DataInterrupt':>14} {'UntilFullyACKed':>16}")
    print(hdr)
    print("  " + "─" * 172)

    for idx, ho in enumerate(handovers):
        label    = LABELS[idx] if idx < len(LABELS) else f"Event{idx+1}"
        route    = f"{apn(ho['from_ap'])}->{apn(ho['to_ap'])}"
        hmode    = ho["mode"]
        lb       = ho["last_beacon"]
        areq     = ho["assoc_req"]
        arsp     = ho["assoc_resp"]
        last_ack = ho.get("last_old_ack")
        fnack    = ho.get("first_new_ack")

        if hmode == "active":
            preq  = ho["trigger"]
            prsp  = ho["phase2_end"]
            ph1   = ms(last_ack, preq)
            ph2   = ms(preq,     prsp)
            ph3   = ms(prsp,     areq)
        else:
            fnb   = ho.get("first_new_beacon")
            ph1   = ms(last_ack, fnb)   # anchor on last valid ACK (same as active)
            ph2   = float('nan')
            ph3   = ms(fnb, areq)

        fdat_s = ho["first_data"]
        ph4    = ms(areq, arsp)
        ph5    = ms(arsp, fdat_s)

        _tot_start = last_ack           # all modes anchored on the last valid ACK
        tot_data = ms(_tot_start, fdat_s)
        tot_ackd = ms(_tot_start, fnack)

        def fc2(v):
            return f"{v:.3f}" if v == v and v is not None else "N/A"

        ho_loss = (str(loss_res['per_ho'][idx])
                   if loss_res and idx < len(loss_res['per_ho']) else "N/A")

        print(f"  {label:<10} {route:<12} {hmode:<8}"
              f"  {fc2(ph1):>13} {fc2(ph2):>13} {fc2(ph3):>13}"
              f"  {fc2(ph4):>12} {fc2(ph5):>12}"
              f"  {ho['flushed_frames']:>11} {ho['lost_est']:>8} {ho_loss:>7}"
              f"  {fc2(tot_data):>14} {fc2(tot_ackd):>16}")

    if loss_res:
        print("  " + "─" * 172)
        print(f"  (Coverage-edge losses, not handover-related: "
              f"{len(loss_res['cats']['coverage-edge'])}   "
              f"Other: {len(loss_res['cats']['other'])})")

    print()

    # ── Optional: STA-log packet-loss classification (detail) ─────────────────
    if loss_res:
        print_loss_classification(loss_res)

print("\nDone.")
