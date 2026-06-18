#include "interferer-application-helper.h"
#include "packet-info.h"
#include "utils.h"
#include "sta-logger.h"
#include "arguments.h"

#include <iostream>
#include <fstream>
#include <sstream>

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
#include "ns3/wifi-mac.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-net-device.h"
#include "my-udp-client-helper.h"
#include "ns3/waypoint-mobility-model.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <unordered_map>

using json = nlohmann::json;

using namespace ns3;


NS_LOG_COMPONENT_DEFINE("LatencyTestV2");

int main(int argc, char** argv) {

    LogComponentEnable("LatencyTestV2", LOG_LEVEL_DEBUG);
    LogComponentEnable("StaLogger", LOG_LEVEL_DEBUG);

    constexpr uint32_t port = 9;
    std::string outFilePath = "db0.json";
    std::string jsonConfig = "conf.json";    
    bool inlineConfig = false;

    CommandLine cmd(__FILE__);
    cmd.AddValue("jsonConfig", "Json configuration", jsonConfig);
    cmd.AddValue("outFilePath", "Output file path", outFilePath);
    cmd.AddValue("inlineConfig", "Provide config inline", inlineConfig);
    cmd.Parse(argc, argv);

    std::cout << jsonConfig << " " << outFilePath << " " << inlineConfig << std::endl;

    // if (argc < 3)
    // {
    //     std::cerr << "Missing parameters" << std::endl;
    //     return 1;
    // }

    Arguments args;

    if(inlineConfig)
    {
        std::stringstream conf_stream(jsonConfig);
        conf_stream >> args;
    }
    else {
        std::ifstream arg_file;
        arg_file.open(jsonConfig.c_str(), std::ios::in);
        if(!arg_file)
        {
            return 2;
        }
        arg_file >> args;
    }


    NS_LOG_DEBUG(args.staNode.mobility.mobilityModel);
    NS_LOG_DEBUG(args.staNode.mobility.startPos.x);
    NS_LOG_DEBUG(args.staNode.mobility.endPos.x);
    NS_LOG_DEBUG(args.staNode.mobility.timeOffset);
    NS_LOG_DEBUG(args.staNode.mobility.tripTime);
    NS_LOG_DEBUG(args.staNode.mobility.repetitions);
    NS_LOG_DEBUG(args.runNumber);


    // Set seed
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(args.runNumber);

    //SeedManager::SetSeed(1);
    //Packet::EnablePrinting();

    // Create node containers for AP, STA and interferers
    NodeContainer wifiApNodes;
    wifiApNodes.Create(args.apNodes.size());
    NodeContainer wifiStaNode;
    wifiStaNode.Create(1);
    NodeContainer wifiInterfererNodes;
    wifiInterfererNodes.Create(args.interfererNodes.size());

    // Create spectrum helpers for different physical configuration
    // (only one will be selected for the simulation)
    std::vector<SpectrumWifiPhyHelper> spectrumPhys;
    for (auto &phyConfig : args.phyConfigs)
    {
        ObjectFactory factory; // used to create objects from their name (useful for json configuration)
        SpectrumWifiPhyHelper spectrumPhyHelper;
        factory.SetTypeId(phyConfig.channel.propagationLossModel);
        Ptr<SpectrumChannel> spectrumChannel = CreateObject<MultiModelSpectrumChannel>();
        Ptr<PropagationLossModel> propagationLossModel = DynamicCast<PropagationLossModel>(factory.Create());
        spectrumChannel->AddPropagationLossModel(propagationLossModel);
        factory.SetTypeId(phyConfig.channel.propagationDelayModel);
        Ptr<PropagationDelayModel> propagationDelayModel = DynamicCast<PropagationDelayModel>(factory.Create());
        spectrumChannel->SetPropagationDelayModel(propagationDelayModel);
        spectrumPhyHelper.SetChannel(spectrumChannel);
        spectrumPhyHelper.Set("ChannelSettings", StringValue(phyConfig.channelSettings));
        spectrumChannel->AssignStreams(100); //allow the deterministic configuration of random variable stream numbers
        spectrumPhys.push_back(spectrumPhyHelper);
    }

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211a);
    WifiMacHelper wifiMac;

    NetDeviceContainer staDevice;
    NetDeviceContainer apDevices;
    NetDeviceContainer interfererDevices;

    // Set station manager (rate adaptation algorithms, e.g. Minstral)
    if (args.staNode.remoteStationManager == "ns3::ConstantRateWifiManager")
    {
        wifi.SetRemoteStationManager(args.staNode.remoteStationManager,
            "MaxSsrc", UintegerValue(21),
            "RtsCtsThreshold", UintegerValue(args.staNode.rtsCtsThreshold),
            "DataMode", StringValue(args.staNode.dataMode));
    }
    else
    {
        wifi.SetRemoteStationManager(args.staNode.remoteStationManager,
            "MaxSsrc", UintegerValue(21),
            "RtsCtsThreshold", UintegerValue(args.staNode.rtsCtsThreshold));
    }

    // Configure WiFi for STA (with beacons)
    // Install on STA node together with spectrum
    wifiMac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(args.staNode.ssid));
    staDevice = wifi.Install(spectrumPhys[args.staNode.phyId], wifiMac, wifiStaNode);

    // Configure and install STA Wifi on interferers
    for (long unsigned int i = 0;i < args.interfererNodes.size();i++)
    {
        const auto& interfererConfig = args.interfererNodes[i];
        const auto wifiInterferentNode = wifiInterfererNodes.Get(i);
        // Use STA WiFi config
        wifiMac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(interfererConfig.ssid));
        // Set rate adaptation algorithm
        if (interfererConfig.remoteStationManager == "ns3::ConstantRateWifiManager")
        {
            wifi.SetRemoteStationManager(interfererConfig.remoteStationManager,
                "MaxSsrc", UintegerValue(21),
                "RtsCtsThreshold", UintegerValue(interfererConfig.rtsCtsThreshold),
                "DataMode", StringValue(interfererConfig.dataMode));
        }
        else
        {
            wifi.SetRemoteStationManager(interfererConfig.remoteStationManager,
                "MaxSsrc", UintegerValue(21),
                "RtsCtsThreshold", UintegerValue(interfererConfig.rtsCtsThreshold));
        }
        interfererDevices.Add(wifi.Install(spectrumPhys[interfererConfig.phyId], wifiMac, wifiInterferentNode));
    }

    // Map SSID to node number for APs
    std::unordered_map<std::string, long unsigned int> apNodesLookup;

    // Configure WiFi for APs
    for (long unsigned int i = 0; i < args.apNodes.size(); i++)
    {
        const auto& apConfig = args.apNodes[i];
        const auto wifiApNode = wifiApNodes.Get(i);
        wifiMac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(apConfig.ssid));
        wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");
        apDevices.Add(wifi.Install(spectrumPhys[apConfig.phyId], wifiMac, wifiApNode));
        apNodesLookup.insert({ apConfig.ssid, i });
    }

    // Enable pcap on all nodes (physical layer)
    if (args.enablePcap)
    {
        for (auto &spectrumPhy : spectrumPhys)
        {
            spectrumPhy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        }
        for (long unsigned int i = 0;i < args.apNodes.size();i++)
        {
            spectrumPhys[args.apNodes[i].phyId].EnablePcap(args.pcapPrefix + "latency-test-ap", apDevices.Get(i));
        }
        for (long unsigned int i = 0;i < args.interfererNodes.size();i++)
        {
            spectrumPhys[args.interfererNodes[i].phyId].EnablePcap(args.pcapPrefix + "latency-test-interferer", interfererDevices.Get(i));
        }
        spectrumPhys[args.staNode.phyId].EnablePcap(args.pcapPrefix + "latency-test-sta", staDevice);
    }

    // Create vector of position for APs, STA, and interferers
    Ptr<ListPositionAllocator> positionAllocator = CreateObject<ListPositionAllocator>();
    for (const auto &apConfig : args.apNodes)
    {
        positionAllocator->Add(Vector(apConfig.position.x, apConfig.position.y, apConfig.position.z));
    }    

    for (const auto &interfererConf : args.interfererNodes)
    {
        positionAllocator->Add(Vector(interfererConf.position.x, interfererConf.position.y, interfererConf.position.z));
    }

    positionAllocator->Add(Vector(args.staNode.position.x, args.staNode.position.y, args.staNode.position.z));

    // Configure mobility model (with type and vector of positions)
    MobilityHelper mobility;  
    mobility.SetPositionAllocator(positionAllocator);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");

    // Install mobility model on nodes (probably goes through the vector of positions)
    mobility.Install(wifiApNodes);
    mobility.Install(wifiInterfererNodes);
    mobility.SetMobilityModel(args.staNode.mobility.mobilityModel);    
    mobility.Install(wifiStaNode);
    Ptr<MobilityModel> staMobilityModel = wifiStaNode.Get(0)->GetObject<MobilityModel>();

    if (args.staNode.mobility.mobilityModel == "ns3::WaypointMobilityModel") {
        Ptr<WaypointMobilityModel> staWaypointMobilityModel = DynamicCast<WaypointMobilityModel>(staMobilityModel);
        if (args.staNode.mobility.timeOffset> 0) {
            staWaypointMobilityModel->AddWaypoint(Waypoint(Seconds(0),Vector(args.staNode.mobility.startPos.x, args.staNode.mobility.startPos.y, args.staNode.mobility.startPos.z)));
        }
        for (uint32_t i = 0; i < args.staNode.mobility.repetitions*2; ++i) {
            Vector waypointsCoords = i % 2 == 0 ?
                Vector(args.staNode.mobility.startPos.x, args.staNode.mobility.startPos.y, args.staNode.mobility.startPos.z):
                Vector(args.staNode.mobility.endPos.x, args.staNode.mobility.endPos.y, args.staNode.mobility.endPos.z);                
            staWaypointMobilityModel->AddWaypoint(
                Waypoint(
                    Seconds(args.staNode.mobility.tripTime * i + args.staNode.mobility.timeOffset),
                    waypointsCoords
                )
            );
        }
    }

    // Install internet stack
    InternetStackHelper internetStack;
    internetStack.Install(wifiApNodes);
    internetStack.Install(wifiStaNode);
    internetStack.Install(wifiInterfererNodes);

    // Configure IP addresses on same subnet
    Ipv4AddressHelper ipv4Address;
    ipv4Address.SetBase("192.168.1.0", "255.255.255.0");
    Ipv4InterfaceContainer apNodesInterfaces;
    Ipv4InterfaceContainer staNodeInterface;
    Ipv4InterfaceContainer interfererNodesInterfaces;

    apNodesInterfaces = ipv4Address.Assign(apDevices);
    staNodeInterface = ipv4Address.Assign(staDevice);
    interfererNodesInterfaces = ipv4Address.Assign(interfererDevices);

    // Install applications on nodes
    ApplicationContainer clientApp;
    ApplicationContainer serverApp;
    ApplicationContainer interfererApps;

    // UDP server on APs (set start time)
    UdpServerHelper server(port);
    serverApp = server.Install(wifiApNodes);
    serverApp.Start(Seconds(1.0));
    serverApp.Stop(Seconds(args.simulationTime + 1));

    // Custom UDP client on STA
    MyUdpClientHelper client(apNodesInterfaces.GetAddress(apNodesLookup[args.staNode.ssid]), port);
    client.SetAttribute("MaxPackets", UintegerValue(4294967295U));
    client.SetAttribute("Interval", TimeValue(Seconds(args.staNode.packetInterval)));
    client.SetAttribute("IntervalJitter", StringValue("ns3::UniformRandomVariable[Min=-0.000025|Max=0.000075]"));
    client.SetAttribute("PacketSize", UintegerValue(args.staNode.payloadSize));
    clientApp = client.Install(wifiStaNode);
    clientApp.Start(Seconds(1.0));
    clientApp.Stop(Seconds(args.simulationTime + 1));

    // Custom UDP client on interferers
    InterfererApplicationHelper interfererHelper;
    for (long unsigned int i = 0;i < args.interfererNodes.size();i++)
    {
        const auto &interfererConfig = args.interfererNodes[i];
        interfererHelper.SetAttribute("PeerAddress", Ipv4AddressValue(apNodesInterfaces.GetAddress(apNodesLookup[interfererConfig.ssid])));
        interfererHelper.SetAttribute("OffTime", StringValue(interfererConfig.offTime));
        interfererHelper.SetAttribute("BurstSize", StringValue(interfererConfig.burstSize));
        interfererHelper.SetAttribute("BurstPacketsInterval", TimeValue(MicroSeconds(interfererConfig.burstPacketsIntervalMicroSeconds)));
        interfererHelper.SetAttribute("BurstPacketsSize", UintegerValue(interfererConfig.burstPacketsSize));
        interfererApps.Add(interfererHelper.Install(wifiInterfererNodes.Get(i)));
    }
    interfererApps.Start(Seconds(1.0));
    interfererApps.Stop(Seconds(args.simulationTime + 1));

    // create STA logger
    std::stringstream ss;
    ss << args;
    STALogger sta_logger(outFilePath, ss.str(), DynamicCast<WifiNetDevice>(staDevice.Get(0)), staMobilityModel);
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

    // Tracing received beacons for both rssi, noise and snr
    ss.str(std::string());
    ss << "/NodeList/" << wifiStaNode.Get(0)->GetId() << "/DeviceList/0/$ns3::WifiNetDevice/Phy/MonitorSnifferRx";
    Config::ConnectWithoutContext(ss.str(), MakeCallback(&STALogger::monitorSnifferRxCallback, &sta_logger));

    // Populate Arp Cache
    PopulateArpCache();

    // Start simulation
    Simulator::Stop(Seconds(args.simulationTime + 1));
    auto start = std::chrono::high_resolution_clock::now();
    Simulator::Run();

    // Log duration of simulation
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(stop - start);

    sta_logger.logFooter(duration);

    Simulator::Destroy();
    return 0;
}