#!/usr/bin/env bash
#
# run_parallel.sh — run the handover scenarios as N independent shards in
# parallel (different RNG runs), then pool all shard logs in the analysis.
#
# NS-3's event engine is single-threaded, so ONE simulation can't be threaded.
# Instead we split the requested round-trips into SHARDS independent replicas
# (each with its own --RngRun), run up to JOBS of them at once, and the
# analysis pools every shard's packets together — statistically equivalent to
# one long run, but ~JOBS times faster in wall-clock.
#
#   ./run_parallel.sh both       # passive + active, sharded
#   ./run_parallel.sh passive
#   ./run_parallel.sh active
#   ./run_parallel.sh analyse    # skip sims, just pool existing shard logs
#
# Key knobs (env vars):
#   TOTAL_TRIPS=10000   total round-trips per scenario (summed over shards)
#   SHARDS=10           how many independent replicas to split into
#   JOBS=10             how many shards to run concurrently (<= CPU cores)
#
set -euo pipefail

NS3_ROOT="${NS3_ROOT:-$HOME/ns-3-dev}"
HANDOVER_DIR="${HANDOVER_DIR:-$NS3_ROOT/scratch/handover2d}"
BIN="${BIN:-$NS3_ROOT/build/handover/ns3-dev-handover-debug}"
OUTDIR="${OUTDIR:-$HANDOVER_DIR/results/handover_parallel}"
ANALYSIS="${ANALYSIS:-$OUTDIR/analysis}"

PASSIVE_CFG="${PASSIVE_CFG:-$HANDOVER_DIR/handover_same_channel_passive_10000.json}"
ACTIVE_CFG="${ACTIVE_CFG:-$HANDOVER_DIR/handover_different_channels_active_10000.json}"

TOTAL_TRIPS="${TOTAL_TRIPS:-10000}"
SHARDS="${SHARDS:-10}"
JOBS="${JOBS:-$SHARDS}"
BINWIDTHS="${BINWIDTHS:-1,2.5,5}"
MODE="${1:-both}"

mkdir -p "$OUTDIR" "$ANALYSIS"
log() { echo -e "\033[1;34m[$(date '+%H:%M:%S')]\033[0m $*"; }

# Build a per-shard config: copy base, override repetitions + simTime.
make_shard_cfg() {
    local base="$1" reps="$2" out="$3"
    python3 - "$base" "$reps" "$out" <<'PY'
import json, sys
base, reps, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
c = json.load(open(base))
c["repetitions"] = reps
c["simTime"] = c["tripTime"] * reps * 2     # exact motion duration, no idle tail
c["enablePcap"] = False
json.dump(c, open(out, "w"), indent=2)
PY
}

# Run all shards of one scenario, JOBS at a time, then echo the log paths.
run_scenario() {
    local tag="$1" base_cfg="$2"
    [ -f "$BIN" ] || { echo "ERROR: binary not found: $BIN"; exit 1; }
    [ -f "$base_cfg" ] || { echo "ERROR: config not found: $base_cfg"; exit 1; }

    # split TOTAL_TRIPS across SHARDS (distribute remainder to the first shards)
    local base=$(( TOTAL_TRIPS / SHARDS ))
    local rem=$(( TOTAL_TRIPS % SHARDS ))

    : > "$OUTDIR/cmds_${tag}.txt"
    for i in $(seq 0 $((SHARDS-1))); do
        local reps=$base
        [ "$i" -lt "$rem" ] && reps=$((reps+1))
        local cfg="$OUTDIR/_shard_${tag}_${i}.json"
        local sta="$OUTDIR/sta_${tag}_shard${i}.json"
        local assoc="$OUTDIR/assoc_${tag}_shard${i}.json"
        make_shard_cfg "$base_cfg" "$reps" "$cfg"
        # --RngRun gives each shard an independent random stream
        echo "cd '$NS3_ROOT' && '$BIN' --jsonConfig='$cfg' --staLogFile='$sta' --assocLogFile='$assoc' --RngRun=$((i+1)) > '$OUTDIR/log_${tag}_shard${i}.txt' 2>&1" \
            >> "$OUTDIR/cmds_${tag}.txt"
    done

    log "$tag: launching $SHARDS shards ($JOBS at a time, $base–$((base+1)) trips each) ..."
    if command -v parallel >/dev/null 2>&1; then
        parallel -j "$JOBS" < "$OUTDIR/cmds_${tag}.txt"
    else
        # xargs fallback: -P parallelism, -L1 one command per line
        xargs -P "$JOBS" -I CMD bash -c CMD < "$OUTDIR/cmds_${tag}.txt"
    fi
    log "$tag: all shards finished."
}

case "$MODE" in
    passive)  run_scenario passive "$PASSIVE_CFG" ;;
    active)   run_scenario active  "$ACTIVE_CFG"  ;;
    both)     run_scenario passive "$PASSIVE_CFG"; run_scenario active "$ACTIVE_CFG" ;;
    analyse|analysis) log "Skipping sims; pooling existing shard logs." ;;
    *) echo "Unknown mode '$MODE' (passive|active|both|analyse)"; exit 1 ;;
esac

# --------------------------------------------------------------------------- #
# Analyse: pool every shard log per scenario (parallel parse with --jobs)
# --------------------------------------------------------------------------- #
log "Analysing (pooling shards) ..."
cd "$HANDOVER_DIR"
ARGS=()
shopt -s nullglob
PASSIVE_LOGS=("$OUTDIR"/sta_passive_shard*.json)
ACTIVE_LOGS=("$OUTDIR"/sta_active_shard*.json)
[ ${#PASSIVE_LOGS[@]} -gt 0 ] && ARGS+=(--passive "${PASSIVE_LOGS[@]}")
[ ${#ACTIVE_LOGS[@]}  -gt 0 ] && ARGS+=(--active  "${ACTIVE_LOGS[@]}")
if [ ${#ARGS[@]} -eq 0 ]; then echo "ERROR: no shard logs found in $OUTDIR"; exit 1; fi

python3 analyze_latency.py "${ARGS[@]}" --binwidths "$BINWIDTHS" \
        --jobs "$JOBS" --outdir "$ANALYSIS"
log "Done. Charts + CSV in: $ANALYSIS"
