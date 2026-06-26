#include "fat-tree-routing.h"

#include "ns3/ipv4-route.h"
#include "ns3/log.h"
#include "ns3/socket.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("FatTreeRouting");
NS_OBJECT_ENSURE_REGISTERED (FatTreeRouting);

TypeId
FatTreeRouting::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::FatTreeRouting")
                          .SetParent<Ipv4RoutingProtocol> ()
                          .SetGroupName ("Internet")
                          .AddConstructor<FatTreeRouting> ();
  return tid;
}

FatTreeRouting::FatTreeRouting ()
    : m_type (0), m_pod (0), m_switchId (0), m_k (4)
{
}

FatTreeRouting::~FatTreeRouting ()
{
}

void
FatTreeRouting::SetSwitchType (uint8_t type, uint8_t pod, uint8_t switchId, uint8_t k)
{
  m_type = type;
  m_pod = pod;
  m_switchId = switchId;
  m_k = k;
}

void
FatTreeRouting::AddPortMapping (uint32_t logicalPort, uint32_t interfaceIndex)
{
  m_portToIf[logicalPort] = interfaceIndex;
}

bool
FatTreeRouting::ComputeOutPort (Ipv4Address dest, uint32_t &logicalPort) const
{
  uint32_t ip = dest.Get ();
  uint8_t destPod = (ip >> 16) & 0xFF;
  uint8_t destSwitch = (ip >> 8) & 0xFF;
  uint8_t destId = ip & 0xFF;
  uint8_t half = m_k / 2;

  if (m_type == 3) // Core: 终止前缀，直接转给目的 Pod
    {
      logicalPort = destPod;
    }
  else if (m_type == 2) // Aggregation
    {
      if (destPod == m_pod)
        {
          logicalPort = destSwitch; // 同 Pod: 下行到对应边缘交换机
        }
      else
        {
          // 跨 Pod: 二级后缀查找，上行到核心 (论文公式)
          logicalPort = ((destId - 2 + m_switchId) % half) + half;
        }
    }
  else if (m_type == 1) // Edge
    {
      if (destPod == m_pod && destSwitch == m_switchId)
        {
          logicalPort = destId - 2; // 本地子网: 下行到主机
        }
      else
        {
          // 出向: 按 (目的主机ID + 本交换机位置) 做后缀散列向上分流。
          // 对应论文 Algorithm 1 的 (i-2+z) mod (k/2) + (k/2)；
          // 注意边缘层(下层 pod 交换机)同样要加位置偏移 m_switchId，
          // 否则同一汇聚交换机上、目的主机ID相同的多条流会撞到同一个核心 -> 只能跑到 50%。
          logicalPort = ((destId - 2 + m_switchId) % half) + half;
        }
    }
  else
    {
      return false; // 未配置类型
    }
  return true;
}

Ptr<Ipv4Route>
FatTreeRouting::BuildRoute (Ipv4Address dest, uint32_t logicalPort) const
{
  std::map<uint32_t, uint32_t>::const_iterator it = m_portToIf.find (logicalPort);
  if (it == m_portToIf.end ())
    {
      return Ptr<Ipv4Route> (); // 该逻辑端口未接线
    }
  uint32_t iface = it->second;

  Ptr<Ipv4Route> route = Create<Ipv4Route> ();
  route->SetDestination (dest);
  route->SetGateway (dest); // 点对点链路无需 ARP，网关填目的即可
  route->SetSource (m_ipv4->GetAddress (iface, 0).GetLocal ());
  route->SetOutputDevice (m_ipv4->GetNetDevice (iface));
  return route;
}

Ptr<Ipv4Route>
FatTreeRouting::RouteOutput (Ptr<Packet>, const Ipv4Header &header,
                             Ptr<NetDevice>, Socket::SocketErrno &sockerr)
{
  Ipv4Address dest = header.GetDestination ();
  uint32_t logicalPort;
  if (!ComputeOutPort (dest, logicalPort))
    {
      sockerr = Socket::ERROR_NOROUTETOHOST;
      return Ptr<Ipv4Route> ();
    }
  Ptr<Ipv4Route> route = BuildRoute (dest, logicalPort);
  if (!route)
    {
      sockerr = Socket::ERROR_NOROUTETOHOST;
      return Ptr<Ipv4Route> ();
    }
  sockerr = Socket::ERROR_NOTERROR;
  return route;
}

bool
FatTreeRouting::RouteInput (Ptr<const Packet> p, const Ipv4Header &header,
                            Ptr<const NetDevice> idev, const UnicastForwardCallback &ucb,
                            const MulticastForwardCallback &, const LocalDeliverCallback &lcb,
                            const ErrorCallback &ecb)
{
  NS_LOG_FUNCTION (this << header.GetDestination ());
  NS_ASSERT (m_ipv4);

  Ipv4Address dest = header.GetDestination ();
  uint32_t iif = m_ipv4->GetInterfaceForDevice (idev);

  // 若目的地址恰好是本机 (交换机一般不会)，本地交付
  for (uint32_t i = 0; i < m_ipv4->GetNInterfaces (); ++i)
    {
      for (uint32_t j = 0; j < m_ipv4->GetNAddresses (i); ++j)
        {
          if (m_ipv4->GetAddress (i, j).GetLocal () == dest)
            {
              lcb (p, header, iif);
              return true;
            }
        }
    }

  uint32_t logicalPort;
  if (!ComputeOutPort (dest, logicalPort))
    {
      ecb (p, header, Socket::ERROR_NOROUTETOHOST);
      return false;
    }
  Ptr<Ipv4Route> route = BuildRoute (dest, logicalPort);
  if (!route)
    {
      ecb (p, header, Socket::ERROR_NOROUTETOHOST);
      return false;
    }
  ucb (route, p, header);
  return true;
}

void
FatTreeRouting::NotifyInterfaceUp (uint32_t)
{
}

void
FatTreeRouting::NotifyInterfaceDown (uint32_t)
{
}

void
FatTreeRouting::NotifyAddAddress (uint32_t, Ipv4InterfaceAddress)
{
}

void
FatTreeRouting::NotifyRemoveAddress (uint32_t, Ipv4InterfaceAddress)
{
}

void
FatTreeRouting::SetIpv4 (Ptr<Ipv4> ipv4)
{
  m_ipv4 = ipv4;
}

void
FatTreeRouting::PrintRoutingTable (Ptr<OutputStreamWrapper> stream, Time::Unit) const
{
  std::ostream *os = stream->GetStream ();
  *os << "[FatTreeRouting] type=" << (uint32_t) m_type << " pod=" << (uint32_t) m_pod
      << " switchId=" << (uint32_t) m_switchId << " k=" << (uint32_t) m_k << "\n";
}

} // namespace ns3
