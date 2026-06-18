#include "interferer-application-helper.h"
#include "packet-info.h"
#include "utils.h"
#include "sta-logger.h"
#include "my-udp-client-helper.h"
#include "assoc-logger.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <unordered_map>

#include "ns3/application-container.h"
#include "ns3/arp-cache.h"
#include "ns3/command-line.h"
#include "ns3/config.h"
#include "ns3/double.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4-address-helper.h"
#include "ns3/ipv4-interface.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/llc-snap-header.h"
#include "ns3/mobility-helper.h"
#include "ns3/multi-model-spectrum-channel.h"
#include "ns3/node-container.h"
#include "ns3/node-list.h"
#include "ns3/object-vector.h"
#include "ns3/pointer.h"
#include "ns3/rng-seed-manager.h"
#include "ns3/seq-ts-header.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/ssid.h"
#include "ns3/string.h"
#include "ns3/udp-client-server-helper.h"
#include "ns3/udp-header.h"
#include "ns3/waypoint-mobility-model.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-net-device.h"
#include "ns3/csma-module.h"
#include "ns3/internet-module.h"
#include "ns3/bridge-helper.h"

#include "ns3/netanim-module.h"
#include "ns3/timer.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("reassociation_test");


struct HandoverConfig {
    bool ENABLE_PCAP = false;
    bool ANIMATION = false;
    double SIM_TIME = 100;
    uint32_t PORT = 9;
    uint32_t PAYLOAD_SIZE = 22;
    double PACKET_INTERVAL = 0.5;
    std::string STA_LOG_PATH = "handover_sta_log.json";
    std::string ASSOC_LOG_PATH = "handover_assoc_log.json";
    // bool inlineConfig = false;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    HandoverConfig, ENABLE_PCAP, ANIMATION, SIM_TIME, PORT, PAYLOAD_SIZE, PACKET_INTERVAL, STA_LOG_PATH, ASSOC_LOG_PATH);


uint16_t STA_CHANNEL = 44;

void timerCallback(Timer* timer, Ptr<MobilityModel> staMobility, Ptr<WifiPhy> phy) {
    NS_LOG_FUNCTION(Simulator::Now().GetSeconds());
    NS_LOG_FUNCTION(staMobility->GetPosition());

    if (STA_CHANNEL == 44) {
        phy->SetAttribute("ChannelSettings", StringValue("{40,20,BAND_5GHZ,0}"));
        STA_CHANNEL = 40;
    }
    else {
        phy->SetAttribute("ChannelSettings", StringValue("{44,20,BAND_5GHZ,0}"));
        STA_CHANNEL = 44;
    }
    timer->Schedule(Seconds(1));
}


int main(int argc, char** argv) {    
    HandoverConfig sim_config;

    LogComponentEnable("reassociation_test", LOG_LEVEL_DEBUG);

    // Set seed
    RngSeedManager::SetSeed(1);
    // Packet::EnablePrinting();

    // Create Wi-Fi nodes
    NodeContainer wifiApNodes;
    wifiApNodes.Create(2);
    NodeContainer wifiStaNode;
    wifiStaNode.Create(1);

    // Adding LAN nodes
    NodeContainer csmaNodes;
    csmaNodes.Add(wifiApNodes); // AP also have csma interface
    csmaNodes.Create(1);

    // Create PHY helper
    SpectrumWifiPhyHelper spectrumPhyHelper;

    Ptr<SpectrumChannel> spectrumChannel = CreateObject<MultiModelSpectrumChannel>();
    Ptr<PropagationLossModel> propagationLossModel = CreateObject<LogDistancePropagationLossModel>();
    Ptr<PropagationDelayModel> propagationDelayModel = CreateObject<ConstantSpeedPropagationDelayModel>();

    spectrumChannel->AddPropagationLossModel(propagationLossModel);
    spectrumChannel->SetPropagationDelayModel(propagationDelayModel);

    spectrumPhyHelper.SetChannel(spectrumChannel);
    spectrumPhyHelper.Set("ChannelSettings", StringValue("{44,20,BAND_5GHZ,0}"));

    // Creater Wi-Fi helper
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211a);
    wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager",
        "MaxSsrc", UintegerValue(21),
        "RtsCtsThreshold", UintegerValue(4692480));
    // wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
    //     "MaxSsrc", UintegerValue(21),
    //     "RtsCtsThreshold", UintegerValue(4692480),
    //     "DataMode", StringValue("OfdmRate6Mbps"));

    // Create mac helper. instal Wifi on nodes to create net devices
    NetDeviceContainer staDevice;
    NetDeviceContainer apDevices;

    WifiMacHelper wifiMac;
    wifiMac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(Ssid("ssid_1")));
    staDevice = wifi.Install(spectrumPhyHelper, wifiMac, wifiStaNode);

    wifiMac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(Ssid("ssid_1")));
    apDevices.Add(wifi.Install(spectrumPhyHelper, wifiMac, wifiApNodes.Get(0)));

    spectrumPhyHelper.Set("ChannelSettings", StringValue("{40,20,BAND_5GHZ,0}"));
    apDevices.Add(wifi.Install(spectrumPhyHelper, wifiMac, wifiApNodes.Get(1)));

    // Configure CSMA
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(NanoSeconds(6560)));
    csma.SetDeviceAttribute("EncapsulationMode", StringValue("Llc"));
    csma.SetDeviceAttribute("Mtu", UintegerValue(1492)); // to be in spec when using LLC (size not more than 1518)
    
    // Create CSMA devices
    NetDeviceContainer csmaDevices;
    csmaDevices = csma.Install(csmaNodes);

    //Configure bridge
    BridgeHelper brh;
    NetDeviceContainer bridgeDevices, toBridgeDevicesAP1, toBridgeDevicesAP2;
    toBridgeDevicesAP1.Add(apDevices.Get(0));
    toBridgeDevicesAP1.Add(csmaDevices.Get(0));
    bridgeDevices = brh.Install(wifiApNodes.Get(0), toBridgeDevicesAP1);
    toBridgeDevicesAP2.Add(apDevices.Get(1));
    toBridgeDevicesAP2.Add(csmaDevices.Get(1));
    bridgeDevices.Add(brh.Install(wifiApNodes.Get(1), toBridgeDevicesAP2));

    // Install internet stack
    InternetStackHelper internetStack;
    internetStack.Install(wifiApNodes);
    internetStack.Install(wifiStaNode);
    internetStack.Install(csmaNodes.Get(2));

    // Create Wi-Fi and LAN subnets
    Ipv4AddressHelper address;

    Ipv4InterfaceContainer apInterfaces, staInterface;
    address.SetBase("192.168.1.0", "255.255.255.0");
    staInterface = address.Assign(staDevice);
    apInterfaces = address.Assign(apDevices);

    Ipv4InterfaceContainer csmaInterfaces;
    //address.SetBase("192.168.2.0", "255.255.255.0");
    csmaInterfaces = address.Assign(csmaDevices);

    // Install applications on nodes
    ApplicationContainer clientApp;
    ApplicationContainer serverApp;

    // UDP server on APs (set start time)
    UdpServerHelper server(sim_config.PORT);
    serverApp = server.Install(csmaNodes.Get(2));
    serverApp.Start(Seconds(1));
    serverApp.Stop(Seconds(sim_config.SIM_TIME + 1));

    // Custom UDP client on STA
    MyUdpClientHelper client(csmaInterfaces.GetAddress(2), sim_config.PORT);
    client.SetAttribute("MaxPackets", UintegerValue(4294967295U));
    client.SetAttribute("Interval", TimeValue(Seconds(sim_config.PACKET_INTERVAL)));
    client.SetAttribute("IntervalJitter", StringValue("ns3::UniformRandomVariable[Min=-0.000025|Max=0.000075]"));
    client.SetAttribute("PacketSize", UintegerValue(sim_config.PAYLOAD_SIZE));
    clientApp = client.Install(wifiStaNode);
    clientApp.Start(Seconds(1));
    clientApp.Stop(Seconds(sim_config.SIM_TIME + 1));

    // Create vector of positions for the two APs
    Ptr<ListPositionAllocator> positionAllocator = CreateObject<ListPositionAllocator>();
    positionAllocator->Add(Vector(50, 0, 0));
    positionAllocator->Add(Vector(100, 0, 0));
    positionAllocator->Add(Vector(60, 0, 0));
    
    // Configure and install mobility for APs
    MobilityHelper mobility;
    mobility.SetPositionAllocator(positionAllocator);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(wifiApNodes);
    mobility.Install(wifiStaNode);

    Ptr<MobilityModel> staMobilityModel = wifiStaNode.Get(0)->GetObject<MobilityModel>();

    // Create STA logger
    std::stringstream ss;
    ss << json(sim_config);
    STALogger sta_logger(sim_config.STA_LOG_PATH, ss.str(), DynamicCast<WifiNetDevice>(staDevice.Get(0)), staMobilityModel);
    sta_logger.logHeader();

    // Tracing for sent packets
    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/$ns3::WifiNetDevice/Phy/PhyTxPsduBegin";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&STALogger::sendingMpduCallback,  &sta_logger));

    // Tracing for acked packets
    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/Mac/AckedMpdu";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&STALogger::ackedMpduCallback, &sta_logger));

    // Tracing for response timeout
    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/Mac/MpduResponseTimeout";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&STALogger::mpduTimeoutCallback, &sta_logger));

    // Tracing for dropped packets
    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/Mac/DroppedMpdu";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&STALogger::droppedMpduCallback, &sta_logger));
    
    // Assoclogger
    AssocLogger assoc_logger(sim_config.ASSOC_LOG_PATH, "{\"header\": \"ADD PARAMETERS\"}",  staMobilityModel);
    assoc_logger.logHeader();
    
    // Tracing new association
    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/Assoc";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&AssocLogger::assocCallback, &assoc_logger));

    // Tracing de-association
    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/DeAssoc";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&AssocLogger::deAssocCallback, &assoc_logger));

    // Tracing beacon arrival
    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/BeaconArrival";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&AssocLogger::beaconArrivalCallback, &assoc_logger));

    // Tracing new beacon info
    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/ReceivedBeaconInfo";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&AssocLogger::receivedBeaconInfoCallback, &assoc_logger));

    // Tracing for mobility (polling)
    Timer timer = Timer();
    timer.SetFunction(&timerCallback);
    timer.SetArguments(&timer, staMobilityModel, DynamicCast<WifiNetDevice>(staDevice.Get(0))->GetPhy());
    timer.Schedule(Seconds(1));

    // Enable pcap on all nodes (physical layer)
    if (sim_config.ENABLE_PCAP)
    {
        spectrumPhyHelper.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        spectrumPhyHelper.EnablePcap("handover-sta", staDevice);
        spectrumPhyHelper.EnablePcap("handover-ap", apDevices.Get(0));
        spectrumPhyHelper.EnablePcap("handover-ap", apDevices.Get(1));
        csma.EnablePcapAll("handover-csma", true);
    }

    // Populate Arp Cache and routing tables
    PopulateArpCache();
    // Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Start simulation
    Simulator::Stop(Seconds(sim_config.SIM_TIME));

    if(sim_config.ANIMATION) {
        AnimationInterface anim ("handover_anim.xml");
    }

    auto start = std::chrono::high_resolution_clock::now();
    Simulator::Run();

    // Log duration of simulation
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(stop - start);
    sta_logger.logFooter(duration);
    assoc_logger.logFooter();

    Simulator::Destroy();
    return 0;
}
