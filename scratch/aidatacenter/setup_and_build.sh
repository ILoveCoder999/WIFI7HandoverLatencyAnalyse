#!/usr/bin/env bash
# Drop the 2D-mesh sim into an ns-3 tree and build it.
# usage: ./setup_and_build.sh /path/to/ns-3-dev
set -euo pipefail
NS3="${1:-}"
[[ -z "$NS3" || ! -d "$NS3" ]] && { echo "usage: $0 /path/to/ns-3-dev"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$NS3/scratch/mesh-2d"
mkdir -p "$DEST"
cp "$HERE"/mesh-sim.cc "$HERE"/mesh-route-header.h "$HERE"/mesh-credit.h \
   "$HERE"/CMakeLists.txt "$DEST"/
echo "copied to $DEST"
cd "$NS3"
./ns3 configure --enable-examples >/dev/null
./ns3 build mesh-sim
echo
echo "run examples:"
echo "  ./ns3 run \"mesh-sim --scenario=uniform\"            # hop distribution sanity"
echo "  ./ns3 run \"mesh-sim --scenario=incast --credit=1 --fanin=64\"  # lossless"
echo "  ./ns3 run \"mesh-sim --scenario=incast --credit=0 --fanin=64\"  # shows loss"
echo "  ./ns3 run \"mesh-sim --scenario=hotspot --credit=1\"  # intersection load"
