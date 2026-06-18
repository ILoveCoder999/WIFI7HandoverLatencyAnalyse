//
// Created by matteo on 04/07/24.
//

#ifndef ARGUMENTS_H
#define ARGUMENTS_H

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#undef NLOHMANN_JSON_FROM_WITH_DEFAULT
#define NLOHMANN_JSON_FROM_WITH_DEFAULT(v1) if (nlohmann_json_j.contains(#v1)) { nlohmann_json_j[#v1].get_to(nlohmann_json_t.v1); } else { nlohmann_json_t.v1 = nlohmann_json_default_obj.v1; }

struct Position
{
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Position,
  x,
  y,
  z)

struct SpectrumChannelConfig
{
  std::string propagationLossModel;
  std::string propagationDelayModel;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SpectrumChannelConfig,
  propagationLossModel,
  propagationDelayModel)

struct SpectrumPhyConfig
{
  std::string channelSettings;
  SpectrumChannelConfig channel;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SpectrumPhyConfig,
  channelSettings,
  channel)

struct ApConfig
{
  Position position{};
  std::string ssid;
  long unsigned int phyId;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ApConfig,
  position,
  ssid,
  phyId)

struct InterfererConfig
{
  Position position{};
  std::string offTime = "ns3::ExponentialRandomVariable[Mean=0.25|Bound=10]";
  std::string burstSize = "ns3::ExponentialRandomVariable[Mean=100|Bound=500]";
  uint64_t burstPacketsIntervalMicroSeconds = 500;
  uint32_t burstPacketsSize = 1500;
  uint32_t rtsCtsThreshold = 4692480;
  std::string remoteStationManager = "ns3::MinstrelHtWifiManager";
  std::string dataMode = "OfdmRate6Mbps";
  std::string ssid;
  long unsigned int phyId;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(InterfererConfig,
  position,
  offTime,
  burstSize,
  rtsCtsThreshold,
  remoteStationManager,
  dataMode,
  ssid,
  phyId)

struct StaMobility
{
  std::string mobilityModel = "ns3::ConstantPositionMobilityModel";  //"ns3::WaypointMobilityModel";
  Position startPos{};
  Position endPos{};  
  double tripTime = 0;
  double timeOffset = 3;
  uint32_t repetitions = 1;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StaMobility,
  mobilityModel,
  startPos,
  endPos,
  tripTime,
  timeOffset,
  repetitions
)

struct StaConfig
{
  Position position{};
  uint32_t payloadSize = 50;
  double packetInterval = 0.5;
  uint32_t rtsCtsThreshold = 4692480;
  std::string remoteStationManager = "ns3::MinstrelHtWifiManager";
  std::string dataMode = "OfdmRate6Mbps";
  std::string ssid;
  long unsigned int phyId;
  StaMobility mobility{};
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StaConfig,
  position,
  payloadSize,
  packetInterval,
  rtsCtsThreshold,
  remoteStationManager,
  dataMode,
  ssid,
  phyId,
  mobility)

struct Arguments
{
  StaConfig staNode{};
  std::vector<SpectrumPhyConfig> phyConfigs{};
  std::vector<ApConfig> apNodes{};
  std::vector<InterfererConfig> interfererNodes{};
  double simulationTime = 10;
  bool enablePcap = false;
  std::string pcapPrefix = "";
  uint64_t runNumber = 1;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Arguments, staNode, phyConfigs, apNodes, interfererNodes, simulationTime, enablePcap, pcapPrefix, runNumber)

inline std::ostream& operator<<(std::ostream& stream, const Arguments& arguments)
{
  return stream << json(arguments);
}

inline std::istream& operator>>(std::istream& stream, Arguments& arguments)
{
  json j;
  stream >> j;
  arguments = j.get<Arguments>();
  return stream;
}

#endif //ARGUMENTS_H
