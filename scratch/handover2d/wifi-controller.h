#pragma once

#include "ns3/wifi-net-device.h"
#include "ns3/bridge-net-device.h"
#include "ns3/mac48-address.h"
#include "ns3/nstime.h"
#include "ns3/net-device.h"

#include <map>

using namespace ns3;

/**
 * WifiController — simulates a centralised WLAN controller (e.g. Cisco WLC).
 *
 * Problem solved
 * --------------
 * After a STA reassociates with AP2, the Linux Bridge on AP2 does not yet know
 * that the STA's MAC is reachable through its WiFi port.  The bridge must wait
 * until the STA sends its first data frame before updating its forwarding table.
 * During this window, every downstream unicast from the server is forwarded via
 * AP1 (stale entry) and flooded by AP2 (unknown unicast), causing measurable
 * extra handover delay.
 *
 * Solution (industry-accurate)
 * ----------------------------
 * When the STA's Assoc trace fires, the controller:
 *   1. Identifies which AP the STA just associated with (from the BSSID).
 *   2. Looks up that AP's BridgeNetDevice and its WiFi-side port.
 *   3. After a configurable propagation delay (modelling the controller →
 *      switch signalling path), calls BridgeNetDevice::ForceLearn() to
 *      directly insert  STA_MAC → WiFi_port  into the forwarding table.
 *
 * The STA does not transmit any extra frame.  This matches the behaviour of
 * real WLC deployments (Cisco WLC, Aruba) where the controller pushes a CAM
 * update to the wired switch at the moment of association.
 *
 * Prerequisites
 * -------------
 * BridgeNetDevice must expose a public ForceLearn() method.
 * Apply bridge-net-device.patch to ns-3's src/bridge/model/ before building.
 */
class WifiController
{
public:
    /**
     * \param staDevice       The STA's WifiNetDevice.
     * \param controllerDelay Simulated controller notification latency.
     *                        Default 1 ms (typical WLC round-trip).
     *                        Set to Time(0) for ideal zero-delay behaviour.
     */
    explicit WifiController(Ptr<WifiNetDevice> staDevice,
                            Time controllerDelay = MilliSeconds(1));

    /**
     * Register an AP so the controller can update the correct bridge.
     * Call once per AP before the simulation starts.
     *
     * \param apDevice  The AP's WifiNetDevice (used to read its BSSID).
     * \param bridge    The BridgeNetDevice installed on the same AP node.
     */
    void registerAp(Ptr<WifiNetDevice>    apDevice,
                    Ptr<BridgeNetDevice>  bridge);

    /**
     * Connect this to the STA's
     *   /NodeList/.../Mac/$ns3::StaWifiMac/Assoc
     * trace source.
     *
     * \param apBssid  BSSID of the AP the STA just associated with
     *                 (passed automatically by the trace).
     */
    void onAssocCallback(Mac48Address apBssid);

private:
    void _forceLearn(Ptr<BridgeNetDevice> bridge, Ptr<NetDevice> wifiPort);
    /** Flush stale BA agreements on the AP side for the returning STA. */
    void _flushApBaForSta(Ptr<NetDevice> apDevice, Mac48Address staMac);

    struct ApEntry {
        Ptr<BridgeNetDevice> bridge;
        Ptr<NetDevice>       wifiPort;   // WiFi-side port of the bridge
    };

    Ptr<WifiNetDevice>              _staDevice;
    Time                            _controllerDelay;
    std::map<Mac48Address, ApEntry> _apRegistry;  // BSSID → bridge + WiFi port
};
