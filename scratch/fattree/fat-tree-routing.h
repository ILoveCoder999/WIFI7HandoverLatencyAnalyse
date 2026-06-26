#ifndef FAT_TREE_ROUTING_H
#define FAT_TREE_ROUTING_H

#include "ns3/ipv4-routing-protocol.h"
#include "ns3/ipv4-address.h"
#include "ns3/ipv4.h"
#include "ns3/output-stream-wrapper.h"
#include "ns3/nstime.h"

#include <map>

namespace ns3 {

/**
 * 论文 "A Scalable, Commodity Data Center Network Architecture" 中的
 * 两层路由表 (two-level routing table) 实现。安装在交换机节点上。
 *
 *   type: 1 = Edge(边缘)   2 = Aggregation(汇聚)   3 = Core(核心)
 *
 * 路由判定只依赖目的主机的私有地址 10.pod.switch.id，
 * 算出一个"逻辑端口号"，再经 m_portToIf 映射到 ns-3 的接口索引。
 */
class FatTreeRouting : public Ipv4RoutingProtocol
{
public:
  static TypeId GetTypeId (void);
  FatTreeRouting ();
  ~FatTreeRouting () override;

  /// 配置该交换机在胖树中的坐标 (建拓扑时调用)
  void SetSwitchType (uint8_t type, uint8_t pod, uint8_t switchId, uint8_t k);
  /// 把"论文逻辑端口号"绑定到 ns-3 的接口索引 (建拓扑时调用)
  void AddPortMapping (uint32_t logicalPort, uint32_t interfaceIndex);

  // ---------------- Ipv4RoutingProtocol 必须实现的接口 ----------------
  Ptr<Ipv4Route> RouteOutput (Ptr<Packet> p, const Ipv4Header &header,
                              Ptr<NetDevice> oif, Socket::SocketErrno &sockerr) override;
  bool RouteInput (Ptr<const Packet> p, const Ipv4Header &header, Ptr<const NetDevice> idev,
                   const UnicastForwardCallback &ucb, const MulticastForwardCallback &mcb,
                   const LocalDeliverCallback &lcb, const ErrorCallback &ecb) override;
  void NotifyInterfaceUp (uint32_t interface) override;
  void NotifyInterfaceDown (uint32_t interface) override;
  void NotifyAddAddress (uint32_t interface, Ipv4InterfaceAddress address) override;
  void NotifyRemoveAddress (uint32_t interface, Ipv4InterfaceAddress address) override;
  void SetIpv4 (Ptr<Ipv4> ipv4) override;
  void PrintRoutingTable (Ptr<OutputStreamWrapper> stream,
                          Time::Unit unit = Time::S) const override;

private:
  /// 解析目的地址 (10.pod.switch.id)，算出应走的逻辑端口
  bool ComputeOutPort (Ipv4Address dest, uint32_t &logicalPort) const;
  /// 根据逻辑端口构造一条 ns-3 路由表项
  Ptr<Ipv4Route> BuildRoute (Ipv4Address dest, uint32_t logicalPort) const;

  uint8_t m_type;     //!< 1=Edge 2=Aggregation 3=Core
  uint8_t m_pod;      //!< 所属 Pod 编号
  uint8_t m_switchId; //!< Pod 内编号
  uint8_t m_k;        //!< 胖树端口数 k
  Ptr<Ipv4> m_ipv4;   //!< 所在节点的 Ipv4 协议栈

  std::map<uint32_t, uint32_t> m_portToIf; //!< 逻辑端口 -> ns-3 接口索引
};

} // namespace ns3

#endif // FAT_TREE_ROUTING_H
