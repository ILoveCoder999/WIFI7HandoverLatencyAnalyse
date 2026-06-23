#!/usr/bin/env bash
# =============================================================================
# run_all.sh — run the 4 handover scenarios and analyze each separately.
#
# For every config it:
#   1. runs the ns-3 binary (--jsonConfig=...),
#   2. saves a per-scenario STA log / assoc log,
#   3. grabs the STA pcap (fixed name, overwritten each run) into a unique name,
#   4. runs analyze_pcap_phases.py on it (pcap + sta-log), saving the report.
#
# Put this script + analyze_pcap_phases.py + the *.json configs all inside
# scratch/handover2d/.  It auto-locates the ns-3 root, so you can launch it
# from anywhere:
#   bash scratch/handover2d/run_all.sh
# =============================================================================
set -u

# ---- auto-locate paths ------------------------------------------------------
# This script lives in scratch/handover2d/ ; ns-3 root is two levels up.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NS3_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$NS3_ROOT"      # ns-3 writes pcaps into the CWD -> must be the ns-3 root

BIN="./build/handover/ns3-dev-handover-debug"   # the freshly-built binary
CONF_DIR="$SCRIPT_DIR"                           # configs sit next to this script
ANALYZER="$SCRIPT_DIR/analyze_pcap_phases.py"    # analysis script
OUTDIR="$SCRIPT_DIR/results"                      # outputs collected here
# -----------------------------------------------------------------------------

SCENARIOS=(
  handover_same_channel_active
  handover_same_channel_passive
  handover_different_channels_active
  handover_different_channels_passive
)

mkdir -p "$OUTDIR"
[ -x "$BIN" ]      || { echo "ERROR: binary not found/executable: $BIN"; exit 1; }
[ -f "$ANALYZER" ] || { echo "ERROR: analyzer not found: $ANALYZER";    exit 1; }

for s in "${SCENARIOS[@]}"; do
  conf="$CONF_DIR/$s.json"
  echo "=================================================================="
  echo " SCENARIO: $s"
  echo "=================================================================="
  if [ ! -f "$conf" ]; then
    echo " [skip] config not found: $conf"; echo; continue
  fi

  # one isolated sub-directory per scenario -> nothing can overwrite anything
  sdir="$OUTDIR/$s"
  mkdir -p "$sdir"
  stalog="$sdir/sta_log.json"
  assoclog="$sdir/assoc_log.json"
  simlog="$sdir/sim.log"

  # remove any stale pcaps in CWD so this run's outputs are unambiguous
  rm -f handover-*.pcap

  # ---- 1. run simulation ----
  echo " [run] $BIN --jsonConfig=$conf"
  "$BIN" --jsonConfig="$conf" \
         --staLogFile="$stalog" \
         --assocLogFile="$assoclog" > "$simlog" 2>&1
  if [ $? -ne 0 ]; then
    echo " [ERROR] simulation failed — see $simlog"; echo; continue
  fi

  # ---- 2. move ALL pcaps (sta / ap0 / ap1 / csma) into the scenario dir ----
  #         (their base names are hard-coded in handover.cc and would otherwise
  #          be overwritten by the next scenario)
  moved=0
  for p in handover-*.pcap; do
    [ -e "$p" ] || continue
    mv "$p" "$sdir/"; moved=$((moved+1))
  done
  stapcap=$(ls -t "$sdir"/handover-sta-*.pcap 2>/dev/null | head -1)
  if [ -z "${stapcap:-}" ]; then
    echo " [ERROR] no STA pcap produced (check $simlog)"; echo; continue
  fi
  echo " [pcap] moved $moved pcap file(s) -> $sdir/"

  # ---- 3. mode + geometry from the config ----
  case "$s" in
    *active*)  mode=active ;;
    *)         mode=passive ;;
  esac
  read trip ps pe intv < <(python3 - "$conf" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(d.get("tripTime", 75),
      d.get("staPosStart", {}).get("x", 0.0),
      d.get("staPosEnd",   {}).get("x", 150.0),
      d.get("packetInterval", 0.03))
PY
)

  # ---- 4. analyze ----
  report="$sdir/analysis.txt"
  python3 "$ANALYZER" "$stapcap" \
          --sta-log "$stalog" --mode "$mode" \
          --trip-time "$trip" --pos-start "$ps" --pos-end "$pe" \
          --packet-interval "$intv" | tee "$report"
  echo " [done] -> $report"
  echo
done

# ---- combined one-line summary (SUMMARY rows from each report) ---------------
echo "=================================================================="
echo " COMBINED SUMMARY (Event rows from each scenario)"
echo "=================================================================="
for s in "${SCENARIOS[@]}"; do
  report="$OUTDIR/$s/analysis.txt"
  [ -f "$report" ] || continue
  echo "--- $s ---"
  awk '/^  Event /{p=1} p&&/Outbound|Return|Event[0-9]/{print} /^Done/{p=0}' "$report"
done

echo
echo "All done. Each scenario isolated in its own folder: $OUTDIR/<scenario>/"
echo "  analysis.txt         — full analysis report"
echo "  sta_log.json         — per-MPDU STA log"
echo "  assoc_log.json       — association log"
echo "  handover-sta-*.pcap  — STA capture  (+ ap0 / ap1 / csma pcaps)"
echo "  sim.log              — ns-3 stdout/stderr"
