#ifndef STA_LOGGER_H
#define STA_LOGGER_H

// standard includes
#include <fstream>
#include <vector>
#include <utility>
#include <string>

// ns3 includes
#include "ns3/pointer.h"
#include "ns3/nstime.h"
#include "ns3/mac48-address.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/he-frame-exchange-manager.h"
#include "ns3/mobility-model.h"
#include "ns3/sta-wifi-mac.h"
#include "ns3/simulator.h"

// custom
#include "packet-info.h"

using namespace ns3;

class STALogger
{
    struct SignalInfo {
        double rssi;
        double noise;
    };

    // Association-history entry: (simulation_time, ap_mac).
    // ap_mac is empty when the STA is disconnected.
    struct AssocEvent {
        Time   time;
        std::string ap; // MAC address string, "" = disconnected
    };

    public:
        STALogger(std::string out_file_path, std::string header,
                  Ptr<WifiNetDevice> net_dev, Ptr<MobilityModel> mobility);
        ~STALogger() {};

        void logHeader();
        void logFooter(std::chrono::seconds);

        /* CALLBACKS — Wi-Fi MAC/PHY traces */
        void ackedMpduCallback(Ptr<const WifiMpdu> mpdu);
        void mpduTimeoutCallback(uint8_t reason, Ptr<const WifiMpdu> mpdu, const WifiTxVector& tx_vector);
        void droppedMpduCallback(WifiMacDropReason reason, Ptr<const WifiMpdu> mpdu);
        void sendingMpduCallback(WifiConstPsduMap psduMap, WifiTxVector txVector, double txPowerW);
        void monitorSnifferRxCallback(
            Ptr<const Packet> packet, uint16_t channelFreqMhz, WifiTxVector txVector,
            MpduInfo aMpdu, SignalNoiseDbm signalNoise, uint16_t staId);

        /* CALLBACKS — association state
         * Connect these to StaWifiMac/Assoc and StaWifiMac/DeAssoc so the logger
         * can reconstruct which AP the STA was associated with at any point in time.
         */
        void assocCallback(Mac48Address ap);
        void deAssocCallback(Mac48Address ap);

    protected:
        std::ofstream _output_file;
        std::string _header;
        Ptr<WifiNetDevice> _net_dev;
        Ptr<MobilityModel> _mobility;
        std::unordered_map<uint32_t, PacketInfo> _packets;
        std::unordered_map<std::string, SignalInfo> _ap_signal;

        // --- AP-association timeline ---
        // Ordered list of association-state changes; used to determine which AP
        // the STA was connected to at any given simulation time.
        std::vector<AssocEvent> _assocHistory;
        // Current AP MAC (empty = disconnected).  Kept in sync with _assocHistory.
        std::string _currentAssocAp;

        /**
         * Returns the AP MAC string the STA was associated with at simulation
         * time @p t, by walking _assocHistory.  Returns "" if disconnected or
         * if no association event has been recorded before @p t.
         */
        std::string _apAtTime(Time t) const;
};

#endif