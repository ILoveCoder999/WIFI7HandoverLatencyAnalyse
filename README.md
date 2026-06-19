# Wi-Fi Handover Simulation & Analysis Toolkit (ns-3 / 802.11be)

An ns-3 dual-AP Wi-Fi 7 handover simulation, with companion pcap/log analysis
scripts and a batch runner. A STA moves back and forth between two APs, triggering
an outbound and a return handover; the tools quantify each handover's per-phase
latency, MAC queue flush behavior, and packet-loss causes.

---

## Contents

All files live in `scratch/handover2d/`:

| File | Description |
|------|-------------|
| `handover.cc` | ns-3 simulation (2 APs + mobile STA + uplink UDP + ForceLearn controller + post-handover BA rebuild) |
| `analyze_pcap_phases.py` | Analyzer: parses the STA capture, splits the handover into phases, reports flush frame counts and packet-loss classification |
| `run_all.sh` | Batch runner: simulates + analyzes the 4 scenarios, isolating each scenario's outputs |
| `handover_same_channel_active.json` | Config: same channel + active scan |
| `handover_same_channel_passive.json` | Config: same channel + passive scan |
| `handover_different_channels_active.json` | Config: different channels + active scan |
| `handover_different_channels_passive.json` | Config: different channels + passive scan |

---

## Requirements

- **ns-3** (with wifi, spectrum, bridge, netanim modules), `scratch/handover2d` set up
- **Python 3** (analyzer uses the standard library only — **no scapy**; pcap is parsed byte-wise)
- **nlohmann/json** (used by `handover.cc` to parse configs)

---

## Build

```bash
cd ~/ns-3-dev
./ns3 build
```

Produces the binary: `build/handover/ns3-dev-handover-debug`

> ⚠️ After editing `handover.cc`, **always rebuild with `./ns3 build`** and run the
> **freshly built** `ns3-dev-handover-debug`. Do not run a stale binary left over
> from an earlier toolchain (e.g. `ns3.48-handover-debug`), or your source changes
> will not take effect.

---

## Running

### Option 1 — batch all 4 scenarios (recommended)

```bash
bash scratch/handover2d/run_all.sh
```

The script auto-locates the ns-3 root and, for each config, runs:

1. The simulation (`--jsonConfig=<config>`), writing uniquely named sta_log / assoc_log;
2. Moves **all** pcaps produced by that run (sta / ap0 / ap1 / csma) into the scenario
   folder (so the next run cannot overwrite them);
3. Runs `analyze_pcap_phases.py` on the STA capture + sta_log;
4. Writes the report to `scratch/handover2d/results/<scenario>/`.

Output layout:

```
results/
└── <scenario>/
    ├── analysis.txt          # full analysis report
    ├── sta_log.json          # per-MPDU log
    ├── assoc_log.json        # association log
    ├── handover-sta-*.pcap   # STA capture (+ ap0 / ap1 / csma)
    └── sim.log               # ns-3 stdout/stderr
```

### Option 2 — run a single scenario manually

```bash
# 1. simulate
./build/handover/ns3-dev-handover-debug \
    --jsonConfig=scratch/handover2d/handover_same_channel_active.json \
    --staLogFile=sta_log.json --assocLogFile=assoc_log.json

# 2. analyze (reads the pcap directly — no Wireshark text export needed)
python3 scratch/handover2d/analyze_pcap_phases.py handover-sta-2-0.pcap \
    --sta-log sta_log.json --mode active
```

> The STA capture is named `handover-sta-<node>-<device>.pcap` (typically
> `handover-sta-2-0.pcap` in this layout). Confirm with `ls handover-sta-*.pcap`.

---

## Analyzer (`analyze_pcap_phases.py`)

### Input

- **pcap** (`.pcap`): parses radiotap + 802.11 frame headers directly from raw bytes,
  compatible with 802.11be/EHT frames;
- **text export** (`.txt`): Wireshark → Export Packet Dissections → As Plain Text (still supported);
- **`--sta-log`** (optional): the ns-3 per-MPDU JSON log, enabling position-based loss classification.

### Handover phases

| Phase | Meaning (active scan / passive scan) |
|-------|--------------------------------------|
| Ph1 Link Failure | last old-AP ACK → Probe Req  /  last old Beacon → first new Beacon |
| Ph2 Channel Probe | Probe Req → Probe Resp (active only) |
| Ph3 Chan Dwell | Probe Resp / first new Beacon → Assoc Req |
| Ph4 Assoc Handshake | Assoc Req → Assoc Resp |
| Ph5 Net Recovery | Assoc Resp → first new data frame |
| Ph5a ADDBA | ADDBA Req → ADDBA Resp |

### Flush / loss statistics (replaces the removed "Phase 6")

> **Why was Phase 6 removed?** The old Phase 6 measured the *time span* (last − first
> timestamp) of the flush burst, which is always ~0 in ns-3: the surviving backlog is
> aggregated into a **single A-MPDU sent at one instant** (span = 0), and when the
> queue fully expires it is also 0. Two completely different situations yielded the
> same 0 — misleading. We now measure the **frame count** instead.

- `FlushFrames` — number of backlogged frames actually transmitted on air after reassoc;
- `LostEst` — estimated loss (≈ estimated queued − flushed);
- `DataInterrupt` — last old-AP ACK → first new data frame (user-perceived outage);
- `UntilFullyACKed` — last old-AP ACK → burst fully acknowledged by the new AP.

### Loss classification (requires `--sta-log`)

Every **never-transmitted** dropped frame is classified by the STA's position at the
frame's generation time:

- **Handover loss** — falls within ±`--ho-window` (default 1.5 s) of a handover instant;
- **Coverage-edge loss** — distance to the nearest AP > `--edge-dist` (default 45 m;
  the effective range in this scenario is ~50 m) — the STA is near the trajectory ends,
  the link is too weak to transmit, and the frame expires in the queue;
- **Other**.

### Common options

```
--mode {auto,active,passive}   scan mode (default auto)
--sta-log <json>               per-MPDU log; enables loss classification
--trip-time / --pos-start / --pos-end / --packet-interval   trajectory & traffic params
--edge-dist <m>                coverage-edge distance threshold (default 45)
--ho-window <s>                handover attribution time window (default 1.5)
--controller-delay <ms>        ForceLearn controller push delay (fixed Phase-5a value)
```

---

## Config fields (`handover.cc` → `HandoverConfig`)

| Field | Default | Description |
|-------|---------|-------------|
| `activeScanning` | true | active vs passive scan |
| `differentChannels` | false | whether the two APs use different channels |
| `tripTime` | 75 | one-way trip time (s) |
| `staPosStart` / `staPosEnd` | 0 / 150 | STA round-trip endpoints (m) |
| `apPositions` | 60, 90 | AP positions (m) |
| `packetInterval` | 0.03 | uplink UDP packet interval (s) |
| `payloadSize` | 22 | payload bytes |
| `maxMissedBeacons` | 3 | missed beacons before declaring link failure |
| `enableController` | false | ForceLearn controller (proactively updates the bridge FDB after handover) |
| `controllerDelayMs` | 1.0 | controller push delay |

---

## Key findings & known behavior

1. **The handover path works correctly**: with an adequate queue lifetime, both the
   outbound and return backlogs flush successfully.
2. **MSDU queue lifetime**: ns-3 `WifiMacQueue::MaxDelay` defaults to 500 ms. When the
   handover outage + queuing time approaches/exceeds it, backlogged real-time packets
   expire and are dropped (realistic behavior). It can be tuned in `handover.cc` after
   `wifi.Install(...)` via `SetAttribute("MaxDelay", ...)` on each AC's queue
   (note: this attribute is **write-only** — do not read it back with `GetAttribute`).
3. **Coverage-edge loss ≠ handover loss**: drops at the trajectory ends (~51 m from the
   AP) are a link-budget limitation, unrelated to the handover; eliminating them
   requires topology changes (denser APs / higher TX power / smaller spacing), not
   software parameters.
4. **Evaluation metrics**: compare handover quality with `FlushFrames` + `DataInterrupt`
   (total outage), not with the time-span "Phase 6".

---

## Troubleshooting

- **`handover.cc` edits have no effect** → not rebuilt, or a stale binary was run.
  Run `./ns3 build`, then execute `build/handover/ns3-dev-handover-debug`.
- **Phase 5 / FlushFrames are all N/A / 0** → data frames not recognized. This analyzer
  parses byte-wise (EHT-compatible); if it still happens, make sure you used the
  **STA-side** pcap (`handover-sta-*.pcap`).
- **The 4 scenarios overwrite each other** → use `run_all.sh`; it runs serially and
  isolates each scenario's outputs into its own folder. Do not chain runs manually
  (the pcap prefix is hard-coded in `handover.cc` and will be overwritten).