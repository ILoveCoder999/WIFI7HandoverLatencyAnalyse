#include "wifi-controller.h"
#include "ns3/ap-wifi-mac.h"
#include "ns3/qos-txop.h"
#include "ns3/block-ack-manager.h"
#include "ns3/qos-utils.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

NS_LOG_COMPONENT_DEFINE("WifiController");

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

WifiController::WifiController(Ptr<WifiNetDevice> staDevice, Time controllerDelay)
    : _staDevice(staDevice),
      _controllerDelay(controllerDelay)
{
    NS_LOG_INFO("WifiController created — controller delay = "
                << controllerDelay.GetMilliSeconds() << " ms");
}

// ---------------------------------------------------------------------------
// AP registration
// ---------------------------------------------------------------------------

void WifiController::registerAp(Ptr<WifiNetDevice>   apDevice,
                                Ptr<BridgeNetDevice> bridge)
{
    // The AP's MAC address is its BSSID — used to match Assoc callbacks.
    Mac48Address bssid = Mac48Address::ConvertFrom(apDevice->GetAddress());

    ApEntry entry;
    entry.bridge   = bridge;
    entry.wifiPort = apDevice;   // the WiFi-side port of the bridge

    _apRegistry[bssid] = entry;

    NS_LOG_INFO("WifiController: registered AP  bssid=" << bssid
                << "  bridge=" << bridge
                << "  wifiPort=" << apDevice);
}

// ---------------------------------------------------------------------------
// Assoc callback — wire to /Mac/$ns3::StaWifiMac/Assoc
// ---------------------------------------------------------------------------

void WifiController::onAssocCallback(Mac48Address apBssid)
{
    auto it = _apRegistry.find(apBssid);
    if (it == _apRegistry.end())
    {
        NS_LOG_WARN("WifiController: Assoc with unknown AP " << apBssid
                    << " — no ForceLearn scheduled");
        return;
    }

    Mac48Address staMac = Mac48Address::ConvertFrom(_staDevice->GetAddress());
    NS_LOG_INFO("[t=" << Simulator::Now().GetMilliSeconds() << "ms] "
                << "WifiController: STA " << staMac
                << " associated with AP " << apBssid
                << " — scheduling ForceLearn in "
                << _controllerDelay.GetMilliSeconds() << " ms");

    // Flush stale BA agreements on the AP side before scheduling ForceLearn.
    _flushApBaForSta(it->second.wifiPort, staMac);

    Simulator::Schedule(_controllerDelay,
                        &WifiController::_forceLearn, this,
                        it->second.bridge,
                        it->second.wifiPort);
}

// ---------------------------------------------------------------------------
// Private: directly update the bridge forwarding table
// ---------------------------------------------------------------------------

void WifiController::_forceLearn(Ptr<BridgeNetDevice> bridge,
                                 Ptr<NetDevice>       wifiPort)
{
    Mac48Address staMac = Mac48Address::ConvertFrom(_staDevice->GetAddress());

    // Directly write STA_MAC → WiFi_port into the bridge's forwarding table.
    // No frame transmitted by STA.  Models a WLC pushing a CAM update to the
    // wired switch at the moment of association.
    bridge->ForceLearn(staMac, wifiPort);

    NS_LOG_INFO("[t=" << Simulator::Now().GetMilliSeconds() << "ms] "
                << "WifiController: ForceLearn("
                << staMac << " → port " << wifiPort << ") applied");
}

// -------------------------------------------------------------------------
// Flush stale BA agreements on the AP side when STA re-associates.
// If AP1 kept an established recipient agreement from the first session,
// ns-3's CreateRecipientAgreement may not overwrite it, causing the
// returning STA's ADDBA exchange to stall silently.
// -------------------------------------------------------------------------
void WifiController::_flushApBaForSta(Ptr<NetDevice> apDevice, Mac48Address staMac)
{
    Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice>(apDevice);
    if (!wifiDev)
    {
        NS_LOG_INFO("[AP-BA-flush] SKIP: apDevice is not WifiNetDevice");
        return;
    }
    Ptr<WifiMac> apMac = wifiDev->GetMac();   // WifiMac has GetQosTxop in ns-3.48
    if (!apMac)
    {
        NS_LOG_INFO("[AP-BA-flush] SKIP: GetMac() returned null");
        return;
    }
    NS_LOG_INFO("[AP-BA-flush] Flushing AP BA for STA " << staMac
                << "  apMac type=" << apMac->GetTypeId().GetName());
    for (uint8_t tid = 0; tid < 8; ++tid)
    {
        AcIndex      ac   = QosUtilsMapTidToAc(tid);
        Ptr<QosTxop> txop = apMac->GetQosTxop(ac);
        if (!txop || !txop->GetBaManager())
            continue;
        auto bam = txop->GetBaManager();
        if (bam->GetAgreementAsOriginator(staMac, tid).has_value())
        {
            NS_LOG_INFO("[AP-BA-flush] originator AP->STA " << staMac
                        << " tid=" << (int)tid << " destroyed");
            bam->DestroyOriginatorAgreement(staMac, tid, std::nullopt);
        }
        NS_LOG_INFO("[AP-BA-flush] recipient STA->AP " << staMac
                    << " tid=" << (int)tid);
        bam->DestroyRecipientAgreement(staMac, tid, std::nullopt);
    }
}
