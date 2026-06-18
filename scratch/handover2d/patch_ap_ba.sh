#!/usr/bin/env bash
# patch_ap_ba.sh — Add AP-side BA flush to WifiController::onAssocCallback
# Run from any directory.  Edits files in ~/ns-3-dev/scratch/handover2d/
set -euo pipefail

CTRL_H=~/ns-3-dev/scratch/handover2d/wifi-controller.h
CTRL_CC=~/ns-3-dev/scratch/handover2d/wifi-controller.cc

# ── 0. sanity check ──────────────────────────────────────────────────────────
[[ -f "$CTRL_H"  ]] || { echo "ERROR: $CTRL_H not found";  exit 1; }
[[ -f "$CTRL_CC" ]] || { echo "ERROR: $CTRL_CC not found"; exit 1; }

# ── 1. wifi-controller.h : declare _flushApBaForSta ─────────────────────────
if grep -q '_flushApBaForSta' "$CTRL_H"; then
    echo "[SKIP] wifi-controller.h already patched"
else
    python3 - "$CTRL_H" << 'PYEOF'
import sys, re
path = sys.argv[1]
src  = open(path).read()
# Insert declaration right after _forceLearn declaration
OLD = '    void _forceLearn(Ptr<BridgeNetDevice> bridge, Ptr<NetDevice> wifiPort);'
NEW = ('    void _forceLearn(Ptr<BridgeNetDevice> bridge, Ptr<NetDevice> wifiPort);\n'
       '    /** Flush stale BA agreements on the AP side for a re-associating STA. */\n'
       '    void _flushApBaForSta(Ptr<WifiNetDevice> apDevice, Mac48Address staMac);')
assert src.count(OLD) == 1, f"anchor not found in .h (count={src.count(OLD)})"
open(path, 'w').write(src.replace(OLD, NEW, 1))
print(f"[OK] wifi-controller.h patched")
PYEOF
fi

# ── 2. wifi-controller.cc : add missing #includes ────────────────────────────
if grep -q 'ap-wifi-mac.h' "$CTRL_CC"; then
    echo "[SKIP] includes already present in wifi-controller.cc"
else
    python3 - "$CTRL_CC" << 'PYEOF'
import sys
path = sys.argv[1]
src  = open(path).read()
# Find the #include "wifi-controller.h" line and append extra headers after it
ANCHOR = '#include "wifi-controller.h"'
assert src.count(ANCHOR) == 1, f"anchor not found (count={src.count(ANCHOR)})"
EXTRA = ('\n#include "ns3/ap-wifi-mac.h"\n'
         '#include "ns3/qos-txop.h"\n'
         '#include "ns3/block-ack-manager.h"\n'
         '#include "ns3/qos-utils.h"')
open(path, 'w').write(src.replace(ANCHOR, ANCHOR + EXTRA, 1))
print("[OK] includes added to wifi-controller.cc")
PYEOF
fi

# ── 3. wifi-controller.cc : call _flushApBaForSta from onAssocCallback ───────
if grep -q '_flushApBaForSta' "$CTRL_CC"; then
    echo "[SKIP] wifi-controller.cc call already present"
else
    python3 - "$CTRL_CC" << 'PYEOF'
import sys
path = sys.argv[1]
src  = open(path).read()

# We insert the flush call immediately before the Simulator::Schedule line
# inside onAssocCallback. We match the exact Schedule call as anchor.
OLD = 'Simulator::Schedule(_controllerDelay,'
# Make sure it's exactly one occurrence  (it should be)
assert src.count(OLD) == 1, f"Schedule anchor count={src.count(OLD)}"

# Replace it with flush + Schedule
NEW = ('// Flush stale BA agreements on the new AP before scheduling ForceLearn.\n'
       '    _flushApBaForSta(it->second.wifiPort, staMac);\n\n'
       '    Simulator::Schedule(_controllerDelay,')
open(path, 'w').write(src.replace(OLD, NEW, 1))
print("[OK] _flushApBaForSta call inserted in onAssocCallback")
PYEOF
fi

# ── 4. wifi-controller.cc : append implementation ────────────────────────────
if grep -q 'WifiController::_flushApBaForSta' "$CTRL_CC"; then
    echo "[SKIP] _flushApBaForSta implementation already present"
else
    cat >> "$CTRL_CC" << 'CPPEOF'

// ---------------------------------------------------------------------------
// _flushApBaForSta: destroy any stale BA agreements on the AP side for the
// returning STA.  Without this, AP1 keeps the ESTABLISHED recipient agreement
// from the first session; ns-3's CreateRecipientAgreement does NOT overwrite
// an existing entry, so the STA's ADDBA exchange on the return trip is silently
// swallowed and zero data frames are ever sent.
// ---------------------------------------------------------------------------
void WifiController::_flushApBaForSta(Ptr<WifiNetDevice> apDevice,
                                       Mac48Address       staMac)
{
    Ptr<ApWifiMac> apMac = DynamicCast<ApWifiMac>(apDevice->GetMac());
    if (!apMac)
        return;

    for (uint8_t tid = 0; tid < 8; ++tid)
    {
        AcIndex      ac   = QosUtilsMapTidToAc(tid);
        Ptr<QosTxop> txop = apMac->GetQosTxop(ac);
        if (!txop || !txop->GetBaManager())
            continue;
        auto bam = txop->GetBaManager();

        // AP as originator (downlink: AP→STA)
        if (bam->GetAgreementAsOriginator(staMac, tid).has_value())
        {
            NS_LOG_INFO("[AP-BA-flush] originator AP→STA " << staMac
                        << " tid=" << static_cast<int>(tid));
            bam->DestroyOriginatorAgreement(staMac, tid, std::nullopt);
        }
        // AP as recipient (uplink: STA→AP) — the stale case on return trip
        NS_LOG_INFO("[AP-BA-flush] recipient STA→AP " << staMac
                    << " tid=" << static_cast<int>(tid));
        bam->DestroyRecipientAgreement(staMac, tid, std::nullopt);
    }
}
CPPEOF
    echo "[OK] _flushApBaForSta implementation appended"
fi

echo ""
echo "All patches applied.  Now rebuild:"
echo "  cd ~/ns-3-dev && ./ns3 build 2>&1 | tail -6"
