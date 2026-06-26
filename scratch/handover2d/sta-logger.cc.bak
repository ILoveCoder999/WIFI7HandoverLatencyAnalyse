#include "sta-logger.h"

#include "ns3/ipv4-l3-protocol.h"
#include "ns3/llc-snap-header.h"
#include "ns3/seq-ts-header.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/udp-header.h"
#include "ns3/udp-l4-protocol.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-net-device.h"

NS_LOG_COMPONENT_DEFINE("StaLogger");

STALogger::STALogger(std::string out_file_path, std::string header,
                     Ptr<WifiNetDevice> net_dev, Ptr<MobilityModel> mobility)
    : _header(header), _net_dev(net_dev), _mobility(mobility)
{
    _output_file.open(out_file_path.c_str(), std::ios::out | std::ios::trunc);
}

// ---------------------------------------------------------------------------
// Association-state tracking
// ---------------------------------------------------------------------------

void STALogger::assocCallback(Mac48Address ap)
{
    std::ostringstream os;
    os << ap;
    _currentAssocAp = os.str();
    _assocHistory.push_back({Simulator::Now(), _currentAssocAp});
    NS_LOG_DEBUG("STALogger: associated with " << _currentAssocAp
                 << " at t=" << Simulator::Now().GetSeconds() << "s");
}

void STALogger::deAssocCallback(Mac48Address /* ap */)
{
    _currentAssocAp = "";
    _assocHistory.push_back({Simulator::Now(), ""});
    NS_LOG_DEBUG("STALogger: de-associated at t=" << Simulator::Now().GetSeconds() << "s");
}

std::string STALogger::_apAtTime(Time t) const
{
    // Walk the history in order; return the last AP recorded at or before t.
    std::string result = "";
    for (const auto& ev : _assocHistory)
    {
        if (ev.time > t) break;
        result = ev.ap;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Log header / footer
// ---------------------------------------------------------------------------

void STALogger::logHeader()
{
    _output_file << "[" << std::endl << _header;
}

void STALogger::logFooter(std::chrono::seconds duration)
{
    _output_file << "," << std::endl << json({
        {"elapsed_seconds", duration.count()}
    }) << std::endl << "]";
}

// ---------------------------------------------------------------------------
// Wi-Fi callbacks
// ---------------------------------------------------------------------------

void STALogger::sendingMpduCallback(WifiConstPsduMap psduMap, WifiTxVector txVector, double txPowerW)
{
    if (psduMap.size() > 1)
    {
        NS_LOG_DEBUG("Psdu map size:" << psduMap.size());
    }
    if (_packets.size() > 1)
    {
        NS_LOG_DEBUG("Tracked packet number:" << _packets.size());
    }

    for (auto& it_psdu : psduMap)
    {
        Ptr<const WifiPsdu> psdu = it_psdu.second;
        if (psdu->GetNMpdus() > 1)
        {
            NS_LOG_DEBUG("Mdpu in psdu: " << psdu->GetNMpdus());
        }
        for (auto it_mpdu : *psdu)
        {
            const WifiMpdu& mpdu = *it_mpdu;
            Ptr<Packet> p = mpdu.GetPacket()->Copy();
            LlcSnapHeader llcSnapHeader;
            Ipv4Header ipv4Header;
            UdpHeader udpHeader;
            SeqTsHeader seqTsHeader;
            if (p->GetSize() >= llcSnapHeader.GetSerializedSize())
            {
                p->RemoveHeader(llcSnapHeader);
            }
            else
            {
                continue;
            }
            // Only process IPv4/UDP packets (skip ARP, 802.11 mgmt, etc.)
            if (llcSnapHeader.GetType() != 0x0800)
            {
                continue;
            }
            if (p->GetSize() >= ipv4Header.GetSerializedSize()
                && p->RemoveHeader(ipv4Header)
                && ipv4Header.GetProtocol() == UdpL4Protocol::PROT_NUMBER
                && p->GetSize() >= udpHeader.GetSerializedSize()
                && p->RemoveHeader(udpHeader)
                && p->GetSize() >= seqTsHeader.GetSerializedSize()
                && p->RemoveHeader(seqTsHeader))
            {
                const auto it = _packets.find(seqTsHeader.GetSeq());
                PacketInfo info{seqTsHeader.GetSeq()};
                bool firstTx = (it == _packets.end());

                if (!firstTx)
                {
                    info = it->second;
                    if (info.current_tx)
                    {
                        NS_LOG_WARN("STALogger: implicit drop"); info.transmissions.push_back(*info.current_tx); info.current_tx = nullptr;
                    }
                }

                std::ostringstream os;
                os << mpdu.GetHeader().GetAddr1();
                info.addr_1 = os.str();

                // --- handover-packet marking (first transmission only) ---
                if (firstTx)
                {
                    // Which AP was the STA associated with when the application
                    // created this packet?  Look it up in the association timeline.
                    info.creation_ap = _apAtTime(seqTsHeader.GetTs());
                    // Which AP is the STA associated with RIGHT NOW (at TX time)?
                    info.first_tx_ap = _currentAssocAp;
                    // Flag: packet crossed an AP boundary (queued on one AP / while
                    // disconnected, transmitted on a different AP).
                    info.handover_packet = (info.creation_ap != info.first_tx_ap);
                    // Time the packet spent waiting in the MAC queue.
                    info.queue_time = Simulator::Now() - seqTsHeader.GetTs();
                }

                // Build per-transmission record
                info.current_tx = std::make_shared<TransmissionInfo>();
                info.current_tx->rate = txVector.GetMode().GetDataRate(txVector);
                info.current_tx->tx_power_w = txPowerW;
                info.current_tx->tx_duration =
                    WifiPhy::CalculateTxDuration(psdu, txVector, _net_dev->GetPhy()->GetPhyBand());
                info.current_tx->assoc_ap = _currentAssocAp;
                if (_ap_signal.find(info.addr_1) != _ap_signal.end())
                {
                    SignalInfo& signalInfo = _ap_signal.at(info.addr_1);
                    info.current_tx->rssi  = signalInfo.rssi;
                    info.current_tx->noise = signalInfo.noise;
                }
                Vector position = _mobility->GetPosition();
                info.current_tx->position = std::make_tuple(position.x, position.y, position.z);
                info.current_tx->tx_time  = Simulator::Now();

                _packets.insert_or_assign(seqTsHeader.GetSeq(), info);
            }
        }
    }
}

void STALogger::ackedMpduCallback(Ptr<const WifiMpdu> mpdu)
{
    Ptr<Packet> p = mpdu->GetPacket()->Copy();
    LlcSnapHeader llcSnapHeader;
    Ipv4Header ipv4Header;
    UdpHeader udpHeader;
    SeqTsHeader seqTsHeader;
    if (p->GetSize() < llcSnapHeader.GetSerializedSize()) return;
    p->RemoveHeader(llcSnapHeader);
    if (llcSnapHeader.GetType() != 0x0800) return;
    if (p->GetSize() >= ipv4Header.GetSerializedSize()
        && p->RemoveHeader(ipv4Header)
        && ipv4Header.GetProtocol() == UdpL4Protocol::PROT_NUMBER
        && p->GetSize() >= udpHeader.GetSerializedSize()
        && p->RemoveHeader(udpHeader)
        && p->GetSize() >= seqTsHeader.GetSerializedSize()
        && p->RemoveHeader(seqTsHeader))
    {
        const auto it = _packets.find(seqTsHeader.GetSeq());
        if (it == _packets.end())
        {
            NS_FATAL_ERROR("Acked packet was not sent");
        }
        PacketInfo info = it->second;
        Time latency = Simulator::Now() - seqTsHeader.GetTs();
        info.acked = true;
        info.latency = latency;
        info.current_tx->latency = latency;
        info.transmissions.push_back(*info.current_tx);
        info.current_tx = nullptr;
        _output_file << ", " << std::endl << json(info);
        _packets.erase(it);
    }
}

// Callback for retransmission timeout
void STALogger::mpduTimeoutCallback(uint8_t /* reason */, Ptr<const WifiMpdu> mpdu, const WifiTxVector& tx_vector)
{
    Ptr<Packet> p = mpdu->GetPacket()->Copy();
    LlcSnapHeader llcSnapHeader;
    Ipv4Header ipv4Header;
    UdpHeader udpHeader;
    SeqTsHeader seqTsHeader;
    if (p->GetSize() < llcSnapHeader.GetSerializedSize()) return;
    p->RemoveHeader(llcSnapHeader);
    if (llcSnapHeader.GetType() != 0x0800) return;
    if (p->GetSize() >= ipv4Header.GetSerializedSize()
        && p->RemoveHeader(ipv4Header)
        && ipv4Header.GetProtocol() == UdpL4Protocol::PROT_NUMBER
        && p->GetSize() >= udpHeader.GetSerializedSize()
        && p->RemoveHeader(udpHeader)
        && p->GetSize() >= seqTsHeader.GetSerializedSize()
        && p->RemoveHeader(seqTsHeader))
    {
        auto it = _packets.find(seqTsHeader.GetSeq());
        if (it == _packets.end())
        {
            NS_FATAL_ERROR("Timeout packet was not sent");
        }
        PacketInfo& info = it->second;
        if (info.current_tx->rate != tx_vector.GetMode().GetDataRate(tx_vector))
        {
            NS_FATAL_ERROR("Rate mismatch on sent packet");
        }
        info.current_tx->latency = Simulator::Now() - seqTsHeader.GetTs();
        info.transmissions.push_back(*info.current_tx);
        info.current_tx = nullptr;
        // Packet stays in _packets for the next sendingMpduCallback (retransmission)
    }
}

// Callback for dropped packet
void STALogger::droppedMpduCallback(WifiMacDropReason reason, Ptr<const WifiMpdu> mpdu)
{
    Ptr<Packet> p = mpdu->GetPacket()->Copy();
    LlcSnapHeader llcSnapHeader;
    Ipv4Header ipv4Header;
    UdpHeader udpHeader;
    SeqTsHeader seqTsHeader;
    if (p->GetSize() < llcSnapHeader.GetSerializedSize()) return;
    p->RemoveHeader(llcSnapHeader);
    if (llcSnapHeader.GetType() != 0x0800) return;
    if (p->GetSize() >= ipv4Header.GetSerializedSize()
        && p->RemoveHeader(ipv4Header)
        && ipv4Header.GetProtocol() == UdpL4Protocol::PROT_NUMBER
        && p->GetSize() >= udpHeader.GetSerializedSize()
        && p->RemoveHeader(udpHeader)
        && p->GetSize() >= seqTsHeader.GetSerializedSize()
        && p->RemoveHeader(seqTsHeader))
    {
        const auto it = _packets.find(seqTsHeader.GetSeq());

        // Packet expired in the MAC queue — was never transmitted
        if (it == _packets.end() && reason == WIFI_MAC_DROP_EXPIRED_LIFETIME)
        {
            std::ostringstream os;
            os << mpdu->GetHeader().GetAddr1();
            PacketInfo info{seqTsHeader.GetSeq()};
            info.addr_1      = os.str();
            info.acked       = false;
            info.latency     = Simulator::Now() - seqTsHeader.GetTs();
            // Mark handover info even for expired-in-queue packets
            info.creation_ap    = _apAtTime(seqTsHeader.GetTs());
            info.first_tx_ap    = "";  // never transmitted
            info.handover_packet = true; // queued but never sent = definitively a handover victim
            info.queue_time     = info.latency; // entire lifetime was queue wait
            _output_file << ", " << std::endl << json(info);
            return;
        }

        if (it == _packets.end())
        {
            NS_FATAL_ERROR("Dropped packet was not sent and did not expire");
        }

        PacketInfo info = it->second;
        Time latency = Simulator::Now() - seqTsHeader.GetTs();
        info.acked = false;
        info.latency = latency;
        if (info.current_tx)
        {
            NS_LOG_DEBUG("Dropping transmission without timeout");
            info.current_tx->latency = latency;
            info.transmissions.push_back(*info.current_tx);
            info.current_tx = nullptr;
        }
        _output_file << ", " << std::endl << json(info);
        _packets.erase(it);
    }
}

void STALogger::monitorSnifferRxCallback(
    Ptr<const Packet> packet, uint16_t /* channelFreqMhz */, WifiTxVector /* txVector */,
    MpduInfo /* aMpdu */, SignalNoiseDbm signalNoise, uint16_t /* staId */)
{
    Ptr<Packet> p = packet->Copy();
    WifiMacHeader wifiMacHeader;
    if (p->GetSize() > 0 && p->RemoveHeader(wifiMacHeader))
    {
        if (wifiMacHeader.IsBeacon())
        {
            std::stringstream ss;
            ss << wifiMacHeader.GetAddr2();
            _ap_signal.insert_or_assign(ss.str(), SignalInfo{signalNoise.signal, signalNoise.noise});
        }
    }
}