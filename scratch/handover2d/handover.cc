#include "interferer-application-helper.h"
#include "packet-info.h"
#include "utils.h"
#include "sta-logger.h"
#include "my-udp-client-helper.h"
#include "assoc-logger.h"
#include "roaming-manager.h"
#include "wifi-controller.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <unordered_map>

#include "ns3/application-container.h"
#include "ns3/command-line.h"
#include "ns3/mobility-helper.h"
#include "ns3/multi-model-spectrum-channel.h"
#include "ns3/node-container.h"
#include "ns3/object-vector.h"
#include "ns3/rng-seed-manager.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/ssid.h"
#include "ns3/string.h"
#include "ns3/udp-client-server-helper.h"
#include "ns3/waypoint-mobility-model.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-net-device.h"
#include "ns3/csma-module.h"
#include "ns3/internet-module.h"
#include "ns3/bridge-helper.h"
#include "ns3/bridge-net-device.h"
#include "ns3/netanim-module.h"
#include "ns3/timer.h"
#include "ns3/qos-txop.h"
#include "ns3/block-ack-manager.h"
#include "ns3/qos-utils.h"
#include "ns3/wifi-mac-queue.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("handover");

struct OutputConfig
{
    std::string STA_LOG_PATH = "handover_sta_log.json";
    std::string ASSOC_LOG_PATH = "handover_assoc_log.json";
};

struct Position
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Interferer {
    Position position = {120, 0, 0};
    std::size_t channel_idx = 0;
};

struct HandoverConfig {
    bool activeScanning = true;
    bool differentChannels = false;
    double simTime = 150;
    std::vector<std::string> channels = {"{36,20,BAND_5GHZ,0}", "{40,20,BAND_5GHZ,0}"};
    std::vector<Interferer> interferers = {};
    double tripTime = 75;
    uint32_t repetitions = 1;
    Position staPosStart = {0, 0, 0};
    Position staPosEnd = {150, 0, 0};
    std::vector<Position> apPositions = {Position{60, 0, 0}, Position{90, 0, 0}};
    uint32_t port = 9;
    uint32_t payloadSize = 22;
    double packetInterval = 0.03;
    bool doubleChannel = false;
    bool constantRate = false;
    bool enablePcap = true;
    bool enableAnimation = false;
    uint32_t maxMissedBeacons = 3;
    // --- Controller (DS forwarding-table proactive update) ---
    // When true, a WifiController sends a gratuitous broadcast frame from the
    // STA immediately after association so that the Linux Bridge on the new AP
    // learns the STA's MAC without waiting for a real data frame.
    bool enableController = false;
    // Simulated controller notification latency in milliseconds.
    // 0 = ideal (instant).  A typical WLC adds ~1–5 ms.
    double controllerDelayMs = 1.0;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Position, x, y, z);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Interferer, position, channel_idx);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    HandoverConfig,
    activeScanning, differentChannels,
    simTime,
    port, payloadSize, packetInterval, doubleChannel, constantRate,
    channels, interferers,
    tripTime, repetitions, staPosStart, staPosEnd,
    apPositions,
    enablePcap, enableAnimation,
    maxMissedBeacons,
    enableController, controllerDelayMs
);

inline std::ostream& operator<<(std::ostream& stream, const HandoverConfig& conf)
{
    return stream << json(conf);
}

inline std::istream& operator>>(std::istream& stream, HandoverConfig& conf)
{
    json j;
    stream >> j;
    conf = j.get<HandoverConfig>();
    return stream;
}

int pos_counter = 0;
void courseChangeCallback(Ptr<const MobilityModel> model) {
    NS_LOG_FUNCTION(pos_counter);
    pos_counter += 1;
}

void timerCallback(Timer* timer, Ptr<WaypointMobilityModel> staMobility) {
    NS_LOG_FUNCTION(Simulator::Now().GetSeconds());
    NS_LOG_FUNCTION(staMobility->GetPosition());
    timer->Schedule(Seconds(1));
}

/**
 * Actual BA teardown — runs deferred via ScheduleNow so that any
 * GetBlockAckReqHeader events already queued at the same simulation
 * timestamp finish before we erase entries from m_originatorAgreements.
 */
// AP the STA just left; BA teardown is deferred until after new Assoc.
static Mac48Address g_pendingBaTeardownAp("ff:ff:ff:ff:ff:ff");

// Call: doDestroyBaAgreements(staMac, oldAp, newAp)
static void doDestroyBaAgreements(Ptr<StaWifiMac> staMac,
                                   Mac48Address oldAp,
                                   Mac48Address newAp)
{
    static const uint8_t acToTids[4][2] = {{0,3},{1,2},{4,5},{6,7}};

    for (uint8_t aci = 0; aci < 4; ++aci)
    {
        Ptr<QosTxop> txop = staMac->GetQosTxop(AcIndex(aci));
        if (!txop || !txop->GetWifiMacQueue())
            continue;
        auto q = txop->GetWifiMacQueue();

        uint32_t dropped = 0, requeued = 0;
        //  std::vector<std::pair<Ptr<Packet>, WifiMacHeader>> toRequeue;
        //  std::vector<std::pair<Ptr<Packet>, WifiMacHeader>> toRequeue;   // 旧
        std::vector<Ptr<WifiMpdu>> toRequeue;                            // 新

        for (uint8_t t = 0; t < 2; ++t)
        {
            uint8_t tid = acToTids[aci][t];
            Ptr<WifiMpdu> mpdu = q->PeekByTidAndAddress(tid, oldAp);
            while (mpdu)
            {
                Ptr<WifiMpdu> next = q->PeekByTidAndAddress(tid, oldAp,
                                                             std::nullopt, mpdu);
                // Re-address ALL packets (in-flight or not) to newAp.
                // In-flight packets have stale SNs from the old BA session with oldAp;
                // removing and re-enqueueing with SN=0 lets BAM assign fresh SNs at TX time.
                // The old AP is gone — no stale ACK will arrive — so this is safe.
                if (mpdu->IsInFlight()) ++dropped;  // count for logging only
                WifiMacHeader newHdr = mpdu->GetHeader();
                newHdr.SetAddr1(newAp);       // RA = newAp
                // 不再写死 SN：让 MAC 在发送时按新 BA 窗口分配窗口内序号
                Ptr<WifiMpdu> m = Create<WifiMpdu>(mpdu->GetPacket()->Copy(), newHdr);
                m->UnassignSeqNo();
                toRequeue.push_back(m);
                q->Remove(mpdu);
                ++requeued;
                mpdu = next;
            }
        }

        // Re-enqueue with newAp addressing (FIFO order preserved)
        //for (auto& [pkt, hdr] : toRequeue)
        //   txop->Queue(Create<WifiMpdu>(pkt, hdr));
        for (auto& m : toRequeue)                          // 新
            txop->Queue(m);

        // ★ 销毁与 oldAp 的 BA originator agreement，停止 BAR 洪流
        for (uint8_t t = 0; t < 2; ++t)
        {
            uint8_t tid = acToTids[aci][t];
            txop->GetBaManager()->DestroyOriginatorAgreement(oldAp, tid, std::nullopt);
        }


        NS_LOG_INFO("[BA-flush] AC " << (uint32_t)aci
                    << ": dropped " << dropped << " in-flight, re-queued "
                    << requeued << " non-in-flight → " << newAp);
    }
}

/**
 * DeAssoc trace callback — schedules the actual teardown via ScheduleNow
 * so same-timestep BAR events (GetBlockAckReqHeader) run first and do not
 * crash on a missing originator agreement.
 *
 * @param staDevice  pre-bound STA WifiNetDevice
 * @param oldAp      BSSID of the AP the STA just left (from trace)
 */
void destroyBaAgreements(Ptr<WifiNetDevice> /*staDevice*/, Mac48Address oldAp)
{
    // Don't tear down immediately — save the old AP and let afterAssocDestroyBa
    // handle it once the STA has associated with the new AP.  Tearing down during
    // scanning corrupts QosTxop state and prevents EDCA access on the return leg.
    NS_LOG_INFO("[BA-flush] DeAssoc from " << oldAp
                << " — deferring BA teardown until new Assoc");
    g_pendingBaTeardownAp = oldAp;
}

/**
 * Assoc trace callback — now that the STA has joined the new AP, tear down
 * the BA agreements with the previous AP.  Running AFTER Assoc avoids
 * flushing QosTxop's pending-MPDU state while the MAC is in scan/auth/assoc
 * limbo, which was causing EDCA access to stall on the return leg.
 */
void afterAssocDestroyBa(Ptr<WifiNetDevice> staDevice, Mac48Address newAp)
{
    if (g_pendingBaTeardownAp.IsGroup())
        return;  // initial association — nothing to clean up
    Mac48Address oldAp = g_pendingBaTeardownAp;
    g_pendingBaTeardownAp = Mac48Address("ff:ff:ff:ff:ff:ff");

    Ptr<StaWifiMac> staMac = DynamicCast<StaWifiMac>(staDevice->GetMac());
    if (!staMac)
        return;
    NS_LOG_INFO("[BA-flush] Assoc complete — now tearing down BA for old AP " << oldAp);
 
    Simulator::ScheduleNow(doDestroyBaAgreements, staMac, oldAp, newAp);
}


int main(int argc, char** argv) {
    LogComponentEnable("RoamingManager",   LOG_LEVEL_DEBUG);
    LogComponentEnable("WifiController",   LOG_LEVEL_DEBUG);

    HandoverConfig sim_config;
    OutputConfig out_config;

    if (argc > 1)
    {
        std::string jsonConfig = "conf.json";
        bool inlineConfig = false;
        CommandLine cmd(__FILE__);
        cmd.AddValue("jsonConfig", "Json configuration", jsonConfig);
        cmd.AddValue("staLogFile", "STA log file path", out_config.STA_LOG_PATH);
        cmd.AddValue("assocLogFile", "Association log file path", out_config.ASSOC_LOG_PATH);
        cmd.AddValue("inlineConfig", "Provide config inline", inlineConfig);
        cmd.Parse(argc, argv);
        std::cout << jsonConfig << " " << out_config.STA_LOG_PATH << " " << out_config.ASSOC_LOG_PATH << " " << inlineConfig << std::endl;
        if (inlineConfig)
        {
            std::stringstream conf_stream(jsonConfig);
            conf_stream >> sim_config;
        }
        else {
            std::ifstream arg_file;
            arg_file.open(jsonConfig.c_str(), std::ios::in);
            if (!arg_file)
            {
                return 1;
            }
            arg_file >> sim_config;
        }
    }
    
    std::cout << "=== Starting simulation with activeScanning = " 
          << sim_config.activeScanning << " ===" << std::endl;
    RngSeedManager::SetSeed(1);

    Config::SetDefault("ns3::WifiMacQueue::MaxDelay", TimeValue(MilliSeconds(800)));

    

    // --- Nodes ---
    NodeContainer wifiApNodes;
    wifiApNodes.Create(2);
    NodeContainer wifiStaNode;
    wifiStaNode.Create(1);

    NodeContainer wifiInterfererNodes;
    wifiInterfererNodes.Create(sim_config.interferers.size());

    NodeContainer csmaNodes;
    csmaNodes.Add(wifiApNodes);
    csmaNodes.Create(1);

    // --- Shared Channel Instance ---
    Ptr<SpectrumChannel> spectrumChannel = CreateObject<MultiModelSpectrumChannel>();
    Ptr<PropagationLossModel> propagationLossModel = CreateObject<LogDistancePropagationLossModel>();
    Ptr<PropagationDelayModel> propagationDelayModel = CreateObject<ConstantSpeedPropagationDelayModel>();
    spectrumChannel->AddPropagationLossModel(propagationLossModel);
    spectrumChannel->SetPropagationDelayModel(propagationDelayModel);

    
    // --- Wi-Fi ---
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211be);  // WiFi 7 (802.11be)
    if (!sim_config.constantRate) {
        // 删除了过时的 MaxSsrc 属性
        wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager",
            "RtsCtsThreshold", UintegerValue(4692480));
    } else {
        // 删除了过时的 MaxSsrc 属性
        wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
            "RtsCtsThreshold", UintegerValue(4692480),
            "DataMode", StringValue("OfdmRate24Mbps"));
    }

    NetDeviceContainer staDevice;
    NetDeviceContainer apDevices;
    NetDeviceContainer interfererDevices;
    WifiMacHelper wifiMac;

    // --- 1. STA PHY & MAC Setup ---
    SpectrumWifiPhyHelper spectrumPhyHelperSta;
    spectrumPhyHelperSta.SetChannel(spectrumChannel);
    spectrumPhyHelperSta.Set("ChannelSettings", StringValue(sim_config.channels.at(0))); // 初始在信道 0 

    // STA MAC 配置
    wifiMac.SetType("ns3::StaWifiMac",
                    "Ssid", SsidValue(Ssid("ssid_1")),
                    "ActiveProbing", BooleanValue(sim_config.activeScanning),
                    "MaxMissedBeacons", UintegerValue(sim_config.maxMissedBeacons),
                    "FrameRetryLimit", UintegerValue(21)); // 在这里统一替代原先的 MaxSsrc
    staDevice = wifi.Install(spectrumPhyHelperSta, wifiMac, wifiStaNode);

    {
        Ptr<StaWifiMac> staMacQ = DynamicCast<StaWifiMac>(
            DynamicCast<WifiNetDevice>(staDevice.Get(0))->GetMac());
        for (auto ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
            Ptr<QosTxop> txop = staMacQ->GetQosTxop(ac);
            if (txop && txop->GetWifiMacQueue()) {
                txop->GetWifiMacQueue()->SetAttribute(
                    "MaxDelay", TimeValue(MilliSeconds(800)));   // 只设置，不回读
                std::cout << "[CHECK] AC=" << ac
                          << " MaxDelay <- 800 ms" << std::endl;
            }
        }
    }


    // --- 2. AP1 PHY & MAC Setup ---
    SpectrumWifiPhyHelper spectrumPhyHelperAp1;
    spectrumPhyHelperAp1.SetChannel(spectrumChannel);
    spectrumPhyHelperAp1.Set("ChannelSettings", StringValue(sim_config.channels.at(0))); // 始终在信道 0 

    wifiMac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(Ssid("ssid_1")));
    apDevices.Add(wifi.Install(spectrumPhyHelperAp1, wifiMac, wifiApNodes.Get(0)));

    // --- 3. AP2 PHY & MAC Setup ---
    SpectrumWifiPhyHelper spectrumPhyHelperAp2;
    spectrumPhyHelperAp2.SetChannel(spectrumChannel);
    if (sim_config.differentChannels) {
        spectrumPhyHelperAp2.Set("ChannelSettings", StringValue(sim_config.channels.at(1))); // 异信道绑定信道 1 
    } else {
        spectrumPhyHelperAp2.Set("ChannelSettings", StringValue(sim_config.channels.at(0))); // 同信道绑定信道 0 
    }
    apDevices.Add(wifi.Install(spectrumPhyHelperAp2, wifiMac, wifiApNodes.Get(1)));

    // --- 4. Interferers Setup ---
    for (std::size_t i = 0; i < sim_config.interferers.size(); i++)
    {
        SpectrumWifiPhyHelper spectrumPhyHelperInt;
        spectrumPhyHelperInt.SetChannel(spectrumChannel);
        spectrumPhyHelperInt.Set("ChannelSettings", StringValue(sim_config.channels.at(sim_config.interferers.at(i).channel_idx)));
        
        wifiMac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(Ssid("ssid_1")));
        interfererDevices.Add(wifi.Install(spectrumPhyHelperInt, wifiMac, wifiInterfererNodes.Get(i)));
    }

    // --- CSMA ---
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(NanoSeconds(6560)));
    csma.SetDeviceAttribute("EncapsulationMode", StringValue("Llc"));
    csma.SetDeviceAttribute("Mtu", UintegerValue(1492));
    NetDeviceContainer csmaDevices = csma.Install(csmaNodes);

    // --- Bridge (AP <-> CSMA) ---
    BridgeHelper brh;
    NetDeviceContainer bridgeDevices, toBridgeDevicesAP1, toBridgeDevicesAP2;
    toBridgeDevicesAP1.Add(apDevices.Get(0));
    toBridgeDevicesAP1.Add(csmaDevices.Get(0));
    bridgeDevices = brh.Install(wifiApNodes.Get(0), toBridgeDevicesAP1);
    toBridgeDevicesAP2.Add(apDevices.Get(1));
    toBridgeDevicesAP2.Add(csmaDevices.Get(1));
    bridgeDevices.Add(brh.Install(wifiApNodes.Get(1), toBridgeDevicesAP2));

    // --- Internet stack ---
    InternetStackHelper internetStack;
    internetStack.Install(wifiApNodes);
    internetStack.Install(wifiStaNode);
    internetStack.Install(wifiInterfererNodes);
    internetStack.Install(csmaNodes.Get(2));

    // --- IP addresses ---
    Ipv4AddressHelper address;
    address.SetBase("192.168.1.0", "255.255.255.0");
    Ipv4InterfaceContainer staInterface  = address.Assign(staDevice);
    Ipv4InterfaceContainer apInterfaces  = address.Assign(apDevices);
    Ipv4InterfaceContainer interfererInterfaces = address.Assign(interfererDevices);
    Ipv4InterfaceContainer csmaInterfaces = address.Assign(csmaDevices);

    // --- Applications ---
    ApplicationContainer serverApp, clientApp, interfererApps;

    UdpServerHelper server(sim_config.port);
    serverApp = server.Install(csmaNodes.Get(2));
    serverApp.Start(Seconds(1));
    serverApp.Stop(Seconds(sim_config.simTime + 1));

    MyUdpClientHelper client(csmaInterfaces.GetAddress(2), sim_config.port);
    client.SetAttribute("MaxPackets", UintegerValue(4294967295U));
    client.SetAttribute("Interval", TimeValue(Seconds(sim_config.packetInterval)));
    client.SetAttribute("IntervalJitter", StringValue("ns3::UniformRandomVariable[Min=-0.000025|Max=0.000075]"));
    client.SetAttribute("PacketSize", UintegerValue(sim_config.payloadSize));
    clientApp = client.Install(wifiStaNode);
    clientApp.Start(Seconds(1));
    clientApp.Stop(Seconds(sim_config.simTime + 1));

    InterfererApplicationHelper interfererHelper;
    for (std::size_t i = 0; i < sim_config.interferers.size(); i++)
    {
        interfererHelper.SetAttribute("PeerAddress", Ipv4AddressValue(csmaInterfaces.GetAddress(2)));
        interfererHelper.SetAttribute("OffTime", StringValue("ns3::ExponentialRandomVariable[Mean=0.25|Bound=10]"));
        interfererHelper.SetAttribute("BurstSize", StringValue("ns3::ExponentialRandomVariable[Mean=100|Bound=500]"));
        interfererHelper.SetAttribute("BurstPacketsInterval", TimeValue(MicroSeconds(500)));
        interfererHelper.SetAttribute("BurstPacketsSize", UintegerValue(1400));
        interfererApps.Add(interfererHelper.Install(wifiInterfererNodes.Get(i)));
    }
    interfererApps.Start(Seconds(1.0));
    interfererApps.Stop(Seconds(sim_config.simTime + 1));

    // --- Mobility ---
    MobilityHelper mobility;

    Ptr<ListPositionAllocator> apPositionAllocator = CreateObject<ListPositionAllocator>();
    for (const auto& apPos : sim_config.apPositions) {
        apPositionAllocator->Add(Vector(apPos.x, apPos.y, apPos.z));
    }
    mobility.SetPositionAllocator(apPositionAllocator);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(wifiApNodes);

    mobility.SetMobilityModel("ns3::WaypointMobilityModel");
    mobility.Install(wifiStaNode);
    Ptr<WaypointMobilityModel> staMobilityModel = DynamicCast<WaypointMobilityModel>(
        wifiStaNode.Get(0)->GetObject<MobilityModel>());
    staMobilityModel->AddWaypoint(Waypoint(Seconds(0),
        Vector(sim_config.staPosStart.x, sim_config.staPosStart.y, sim_config.staPosStart.z)));
    for (uint32_t i = 0; i < sim_config.repetitions; ++i) {
        staMobilityModel->AddWaypoint(Waypoint(Seconds(sim_config.tripTime * (i*2 + 1)),
            Vector(sim_config.staPosEnd.x, sim_config.staPosEnd.y, sim_config.staPosEnd.z)));
        staMobilityModel->AddWaypoint(Waypoint(Seconds(sim_config.tripTime * (i*2 + 2)),
            Vector(sim_config.staPosStart.x, sim_config.staPosStart.y, sim_config.staPosStart.z)));
    }

    Ptr<ListPositionAllocator> intPositionAllocator = CreateObject<ListPositionAllocator>();
    for (const auto& inf : sim_config.interferers) {
        intPositionAllocator->Add(Vector(inf.position.x, inf.position.y, inf.position.z));
    }
    mobility.SetPositionAllocator(intPositionAllocator);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(wifiInterfererNodes);

    // --- Loggers ---
    std::stringstream ss;
    ss << json(sim_config);
    STALogger sta_logger(out_config.STA_LOG_PATH, ss.str(),
        DynamicCast<WifiNetDevice>(staDevice.Get(0)), staMobilityModel);
    sta_logger.logHeader();

    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/$ns3::WifiNetDevice/Phy/PhyTxPsduBegin";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&STALogger::sendingMpduCallback, &sta_logger));

    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/Mac/AckedMpdu";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&STALogger::ackedMpduCallback, &sta_logger));

    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/Mac/MpduResponseTimeout";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&STALogger::mpduTimeoutCallback, &sta_logger));

    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/Mac/DroppedMpdu";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&STALogger::droppedMpduCallback, &sta_logger));

    AssocLogger assoc_logger(out_config.ASSOC_LOG_PATH, "{\"header\": \"ADD PARAMETERS\"}", staMobilityModel);
    assoc_logger.logHeader();

    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/Assoc";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&AssocLogger::assocCallback, &assoc_logger));

    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/DeAssoc";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&AssocLogger::deAssocCallback, &assoc_logger));

    // --- Block Ack session flush on de-association ---
    // Without this the BlockAckManager keeps the old BA agreement alive and
    // fires ~1 800 Block Ack Req frames to the now-unreachable AP, blocking
    // the new ADDBA handshake for ~435 ms.
    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId()
       << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/DeAssoc";
    Config::ConnectWithoutContext(ss.str(),
        MakeBoundCallback(&destroyBaAgreements,
                          DynamicCast<WifiNetDevice>(staDevice.Get(0))));

    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/ReceivedBeaconInfo";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&AssocLogger::receivedBeaconInfoCallback, &assoc_logger));

    // Mobility polling
    Timer timer = Timer();
    timer.SetFunction(&timerCallback);
    timer.SetArguments(&timer, staMobilityModel);
    timer.Schedule(Seconds(1));

    // Channel-change roaming manager
    RoamingManager roaming_manager(DynamicCast<WifiNetDevice>(staDevice.Get(0)),
                                   staMobilityModel, sim_config.channels);
    if (sim_config.differentChannels) {
        ss.str(std::string());
        ss << "/NodeList/" << wifiStaNode.Get(0)->GetId()
           << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/DeAssoc";
        Config::ConnectWithoutContext(ss.str(),
            MakeCallback(&RoamingManager::deAssocCallback, &roaming_manager));
        // 连接 beacon 信息给 RoamingManager
        ss.str(std::string());
        ss << "/NodeList/" << wifiStaNode.Get(0)->GetId()
           << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/ReceivedBeaconInfo";
        Config::ConnectWithoutContext(ss.str(),
            MakeCallback(&RoamingManager::receivedBeaconInfoCallback, &roaming_manager));
    }

    // --- WifiController (proactive DS forwarding-table update) ---
    // Declared outside the if-block so it remains alive for the simulation.
    std::unique_ptr<WifiController> wifi_controller;
    if (sim_config.enableController)
    {
        std::cout << "=== WifiController ENABLED  (delay="
                  << sim_config.controllerDelayMs << " ms) ===" << std::endl;
        wifi_controller = std::make_unique<WifiController>(
            DynamicCast<WifiNetDevice>(staDevice.Get(0)),
            MilliSeconds(static_cast<int64_t>(sim_config.controllerDelayMs)));

        // Register both APs: controller needs their BSSID → (bridge, wifiPort)
        // mapping so it knows which bridge to update when Assoc fires.
        wifi_controller->registerAp(
            DynamicCast<WifiNetDevice>(apDevices.Get(0)),
            DynamicCast<BridgeNetDevice>(bridgeDevices.Get(0)));
        wifi_controller->registerAp(
            DynamicCast<WifiNetDevice>(apDevices.Get(1)),
            DynamicCast<BridgeNetDevice>(bridgeDevices.Get(1)));

        ss.str(std::string());
        ss << "/NodeList/" << wifiStaNode.Get(0)->GetId()
           << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/Assoc";
        Config::ConnectWithoutContext(ss.str(),
            MakeCallback(&WifiController::onAssocCallback, wifi_controller.get()));

        // Deferred BA teardown: destroy old-AP agreements after new Assoc
        ss.str(std::string());
        ss << "/NodeList/" << wifiStaNode.Get(0)->GetId()
           << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/Assoc";
        Config::ConnectWithoutContext(ss.str(),
            MakeBoundCallback(&afterAssocDestroyBa,
                              DynamicCast<WifiNetDevice>(staDevice.Get(0))));
    }
    else
    {
        std::cout << "=== WifiController DISABLED ===" << std::endl;
    }

    // --- PCAP ---
    if (sim_config.enablePcap)
    {
        // 显式分别对各 PHY Helper 开启 PCAP 以防挂载不全 
        spectrumPhyHelperSta.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        spectrumPhyHelperSta.EnablePcap("handover-sta", staDevice);
        
        spectrumPhyHelperAp1.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        spectrumPhyHelperAp1.EnablePcap("handover-ap0", apDevices.Get(0));
        
        spectrumPhyHelperAp2.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        spectrumPhyHelperAp2.EnablePcap("handover-ap1", apDevices.Get(1));
        
        csma.EnablePcapAll("handover-csma", true);
    }

    // --- Run ---
    PopulateArpCache();
    Simulator::Stop(Seconds(sim_config.simTime));

    if (sim_config.enableAnimation) {
        AnimationInterface anim("handover_anim.xml");
    }

    auto start = std::chrono::high_resolution_clock::now();
    Simulator::Run();

    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(stop - start);
    sta_logger.logFooter(duration);
    assoc_logger.logFooter();

    Simulator::Destroy();
    return 0;
}