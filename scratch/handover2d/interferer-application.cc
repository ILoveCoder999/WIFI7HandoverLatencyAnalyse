//
// Created by matteo on 29/05/24.
//

#include "interferer-application.h"

#include "ns3/udp-socket-factory.h"
#include "ns3/pointer.h"
#include "ns3/seq-ts-header.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("InterfererApplication");

NS_OBJECT_ENSURE_REGISTERED(InterfererApplication);

InterfererApplication::InterfererApplication()
    : m_burstPacketsSize(1500),
      m_socket(nullptr)
{
    NS_LOG_FUNCTION(this);
}

InterfererApplication::~InterfererApplication()
{
    NS_LOG_FUNCTION(this);
}

TypeId
InterfererApplication::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::InterfererApplication")
            .SetParent<Application>()
            .SetGroupName("Applications")
            .AddConstructor<InterfererApplication>()
            .AddAttribute("OffTime",
                          "The RandomVariableStream for the time off",
                          StringValue("ns3::ExponentialRandomVariable[Mean=0.25|Bound=10]"),
                          MakePointerAccessor(&InterfererApplication::m_offTime),
                          MakePointerChecker<RandomVariableStream>())
            .AddAttribute("BurstSize",
                          "The RandomVariableStream for the number of packets in the burst",
                          StringValue("ns3::ExponentialRandomVariable[Mean=100|Bound=500]"),
                          MakePointerAccessor(&InterfererApplication::m_burstSize),
                          MakePointerChecker<RandomVariableStream>())
            .AddAttribute("BurstPacketsInterval",
                          "The Time interval between packets in the burst",
                          TimeValue(MicroSeconds(500)),
                          MakeTimeAccessor(&InterfererApplication::m_burstPacketsInterval),
                          MakeTimeChecker())
            .AddAttribute("BurstPacketsSize",
                          "The size of packets in the burst",
                          UintegerValue(1500),
                          MakeUintegerAccessor(&InterfererApplication::m_burstPacketsSize),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("PeerAddress",
                          "The peer address",
                          Ipv4AddressValue(),
                          MakeIpv4AddressAccessor(&InterfererApplication::m_peerAddress),
                          MakeIpv4AddressChecker());
    return tid;
}

void
InterfererApplication::StartApplication()
{
    m_packetNumber = 0;
    if (!m_socket)
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind();
        m_socket->Connect(InetSocketAddress(m_peerAddress, 9));
    }
    m_currentBurstSize = m_burstSize->GetInteger();
    m_sendEvent = Simulator::Schedule(Seconds(m_offTime->GetValue()), &InterfererApplication::SendPacket, this);
}

void
InterfererApplication::StopApplication()
{
    m_sendEvent.Cancel();
    if (m_socket)
    {
        m_socket->Close();
    }
}

void
InterfererApplication::SendPacket()
{
    NS_LOG_FUNCTION(this);

    SeqTsHeader seqTs;
    seqTs.SetSeq(m_packetNumber++);
    Ptr<Packet> packet = Create<Packet>(m_burstPacketsSize - seqTs.GetSerializedSize() - 28);
    packet->AddHeader(seqTs);
    m_socket->Send(packet);
    if (m_currentBurstSize == 0 || m_packetNumber % m_currentBurstSize == 0)
    {
        // Burst finisched, scheduling next burst
        m_currentBurstSize = m_burstSize->GetInteger();
        m_sendEvent = Simulator::Schedule(Seconds(m_offTime->GetValue()), &InterfererApplication::SendPacket, this);
    }
    else
    {
        // Schedule next packet
        m_sendEvent = Simulator::Schedule(m_burstPacketsInterval, &InterfererApplication::SendPacket, this);
    }
}

} // ns3