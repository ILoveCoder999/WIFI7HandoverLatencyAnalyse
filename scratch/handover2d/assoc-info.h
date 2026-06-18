#ifndef ASSOC_INFO_H
#define ASSOC_INFO_H

#include "ns3/sta-wifi-mac.h"
#include "ns3/wifi-mac.h"
#include "str-utils.h"
#include <sstream>

#include <nlohmann/json.hpp>
#include <ostream>

using json = nlohmann::json;


// This is used to serialize third party types without adding changes to the library (ns-3 in this case)
NLOHMANN_JSON_NAMESPACE_BEGIN
template<>
struct adl_serializer<WifiScanParams::Channel> {
    static void to_json(json& j, const WifiScanParams::Channel& c) {
        std::stringstream ss;
        ss << c.number;
        std::string ch_num = ss.str();

        ss.str(std::string());
        ss << c.band;
        std::string band = ss.str();

        j = json {
            {"number", ch_num},
            {"band", band}
        };
    }
};

template<>
struct adl_serializer<StaWifiMac::ApInfo> {    
    static void to_json(json& j, const StaWifiMac::ApInfo& a) {
        std::stringstream ss;    
        ss << a.m_bssid;
        std::string bssid = ss.str();

        ss.str(std::string());
        ss << a.m_apAddr;
        std::string ap_addr = ss.str();    

        std::ostringstream os;
        std::visit([&os](auto&& frame) { frame.Print(os); }, a.m_frame);
        std::string frame = os.str();

        auto frame_list = str_utils::split_str(frame, std::string(",")); // split on ","
        std::for_each(frame_list.begin(), frame_list.end(), [](std::string & s){str_utils::trim(s);}); // remove spaces
        frame_list.erase(std::remove(frame_list.begin(), frame_list.end(), ""), frame_list.end()); // remove empty

        json j_frame = json {};
        for (auto &s : frame_list) {
            auto t = str_utils::split_str(s, std::string("=")); // split on "="
            std::string& key = t[0];
            std::string& val = t[1];
            if (key == "ssid") {
                j_frame["ssid"] = t[1];
            }
            else if (key == "rates") {
                val = val.substr(1, val.size() - 2);
                j_frame["rates"] = str_utils::split_str(val, std::string(" "));
            }
        }

        j = json {
                {"bssid", bssid},
                {"apAddr", ap_addr},
                {"snr", a.m_snr},
                {"channel", a.m_channel},
                {"link_id", a.m_linkId},
                {"frame_orign", frame},
                {"frame", j_frame}
        };
    }
};
NLOHMANN_JSON_NAMESPACE_END

enum class BeaconInfoType { arrival, info };
enum class AssocInfoType { assoc, deassoc };

std::vector<std::string> BeaconInfoTypeToStr = {"BeaconArrival", "BeaconInfo"};
std::vector<std::string> AssocInfoTypeToStr = {"Association", "De-association"};

struct AssocInfo
{
    AssocInfoType msg_type{};
    ns3::Time tx_time{};
    std::tuple<double, double, double> position{};
    Mac48Address addr {};
};

struct BeaconInfo
{
    BeaconInfoType msg_type {};
    ns3::Time tx_time{};
    std::tuple<double, double, double> position{};
    StaWifiMac::ApInfo apInfo{};    
};

inline void to_json(json& j, const AssocInfo a)
{
    std::stringstream ss; 
    ss << a.addr;
    std::string addr = ss.str(); 
    j = json {
            {"msg", AssocInfoTypeToStr.at(static_cast<int>(a.msg_type))},
            {"tx_time", a.tx_time.ToInteger(ns3::Time::Unit::NS)},
            {"position", a.position},
            {"ap_info", addr}
    };
}

inline void to_json(json& j, const BeaconInfo a)
{
    j = json {
            {"msg", BeaconInfoTypeToStr.at(static_cast<int>(a.msg_type))},
            {"tx_time", a.tx_time.ToInteger(ns3::Time::Unit::NS)},
            {"position", a.position}
    };
    if (a.msg_type == BeaconInfoType::info) {
        j["ap_info"] = a.apInfo;
    }
}
#endif //ASSOC_INFO_H
