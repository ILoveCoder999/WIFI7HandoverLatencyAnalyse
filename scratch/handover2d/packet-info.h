//
// Created by matteo on 23/07/24.
//

#ifndef PACKET_INFO_H
#define PACKET_INFO_H

#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct TransmissionInfo
{
    uint64_t rate;
    ns3::Time latency;
    ns3::Time tx_duration;
    double tx_power_w;
    std::tuple<double, double, double> position;
    ns3::Time tx_time;
    double rssi;
    double noise;
    // MAC address of the AP the STA was associated with during this TX attempt
    // (empty string = disconnected / unknown)
    std::string assoc_ap = "";
};

inline void to_json(json& j, const TransmissionInfo& r)
{
    j = json {
            {"rate",         r.rate},
            {"latency",      r.latency.ToInteger(ns3::Time::Unit::NS)},
            {"tx_duration",  r.tx_duration.ToInteger(ns3::Time::Unit::NS)},
            {"tx_power_w",   r.tx_power_w},
            {"position",     r.position},
            {"tx_time",      r.tx_time.ToInteger(ns3::Time::Unit::NS)},
            {"rssi",         r.rssi},
            {"noise",        r.noise},
            {"assoc_ap",     r.assoc_ap}
    };
}

struct PacketInfo
{
    uint32_t seq;
    bool acked;
    ns3::Time latency;
    std::vector<TransmissionInfo> transmissions;
    std::shared_ptr<TransmissionInfo> current_tx;
    std::string addr_1 = "";

    // --- handover-packet marking ---
    // AP the STA was associated with when the packet was CREATED by the application
    // (derived from seqTsHeader.GetTs() vs the association-history timeline).
    // Empty string means the STA was disconnected (or not yet seen an assoc event).
    std::string creation_ap = "";

    // AP the STA was associated with when the packet was FIRST TRANSMITTED.
    // Empty string means the STA was disconnected at TX time.
    std::string first_tx_ap = "";

    // True when creation_ap != first_tx_ap, i.e. the packet spent time in the
    // MAC queue across an AP boundary (either queued while disconnected, or queued
    // on AP1 and sent on AP2).  In this case:
    //   queue_time  = time in MAC queue (creation → first TX attempt)
    //   latency     = total end-to-end latency (creation → ACK / drop)
    //   TX latency  = latency - queue_time
    bool handover_packet = false;

    // Time the packet spent waiting in the MAC queue before its first transmission
    // attempt.  For normal packets this is effectively 0 (sub-ms processing delay).
    ns3::Time queue_time;
};

inline void to_json(json& j, const PacketInfo& p)
{
    j = json {
            {"seq",              p.seq},
            {"acked",            p.acked},
            {"latency",          p.latency.ToInteger(ns3::Time::Unit::NS)},
            {"queue_time",       p.queue_time.ToInteger(ns3::Time::Unit::NS)},
            {"handover_packet",  p.handover_packet},
            {"creation_ap",      p.creation_ap},
            {"first_tx_ap",      p.first_tx_ap},
            {"transmissions",    p.transmissions},
            {"addr_1",           p.addr_1}
    };
}

#endif //PACKET_INFO_H