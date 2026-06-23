/*
 * dragonfly-ib-sim.cc
 *
 * Dragonfly 拓扑 + InfiniBand NDR 仿真
 * ──────────────────────────────────────
 * 拓扑（Kim et al. 2008 平衡 Dragonfly）
 * ─────────────────────────────────────
 *   · a   = 每个 Group 内的 Router 数量
 *   · p   = 每台 Router 下连接的 Server 数量
 *   · h   = 每台 Router 的 Global Link 数（跨 Group）
 *   · g   = Group 总数 = a×h + 1（全连接时恰好每对 Group 间有 1 条链路）
 *   · N   = 总服务器数 = p × a × g
 *
 *   默认参数（a=p=h=6）：
 *     g = 6×6+1 = 37 个 Group
 *     N = 6×6×37 = 1332 台服务器   ← 与 3D Mesh (1331) 规模对齐
 *     Router 数量 = a×g = 222
 *     直径 = 3 跳（同 Group 内 ≤2 router hops，跨 Group ≤3 router hops）
 *
 * 链路结构
 * ──────────
 *   1. Server ↔ Router      : p×a×g  = 1332 条（每台服务器直连其 Router）
 *   2. Intra-Group（局部链路）: a(a-1)/2×g = 555 条（Group 内 Router 全互联）
 *   3. Inter-Group（Global Link）: g(g-1)/2 = 666 条（每对 Group 间 1 条）
 *      全局链路分配（平衡，每 Router 恰好 h=a 条 Global Link）：
 *        对于 Group 对 (i, j)（i < j）：
 *          Group i 的 Router r_i = (j−i−1) mod a 负责此链路
 *          Group j 的 Router r_j = (g−(j−i)−1) mod a 负责此链路
 *      → 每 Router 恰好 h=6 条 Global Link，拓扑完全平衡 ✓
 *
 * InfiniBand 特性近似
 * ─────────────────────
 *   · 链路速率 400 Gbps（InfiniBand NDR，可命令行覆盖）
 *   · 无损传输：深队列（queuePkts=256）近似 IB credit-based 流控（非 ECN/PFC）
 *     注：IB 硬件使用 credit 计数器保证无丢包，ns3 无原生支持；
 *         深队列使丢包概率极低，是常见仿真近似手段
 *   · 路由：GlobalRouting + RandomEcmpRouting，近似 IB UGAL 自适应路由
 *     UGAL 在最短路径和 Valiant 非最短路径之间自适应选择；
 *     此处用随机 ECMP 在多条等价最短路径间分流，轻负载行为一致
 *
 * 流量场景（--scenario=）
 * ─────────────────────────
 *   uniform   : 均匀随机打流，基准带宽测试
 *   incast    : 多对一（模拟 AllReduce 的 Gather 阶段），测 Router 收敛
 *   allreduce : 逻辑环 ring-allreduce（DL 训练 reduce-scatter 单步通信）
 *   bisection : 前半 Group 打后半 Group，测 Global Link 平分带宽
 */

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/ipv4-global-routing-helper.h"

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("DragonflyIbSim");

// ─── 拓扑常量 ──────────────────────────────────────────────────────────────
static const uint32_t A = 6;           // routers per group
static const uint32_t P = 6;           // servers per router
static const uint32_t H = A;           // global links per router (balanced: H=A)
static const uint32_t G = A * H + 1;   // groups = 37
static const uint32_t N = P * A * G;   // total servers = 1332

// ─── 节点索引辅助 ────────────────────────────────────────────────────────
// Router 在 routerNodes 中的索引
static uint32_t routerIdx (uint32_t g, uint32_t r) { return g * A + r; }
// Server 在 serverNodes 中的索引
static uint32_t serverIdx (uint32_t g, uint32_t r, uint32_t s)
{ return (g * A + r) * P + s; }

// ─── 全局统计 ──────────────────────────────────────────────────────────────
struct Probe { double sentNs; double recvNs; };
static std::map<uint64_t, Probe> g_inflight;
static std::vector<Probe>        g_done;
static uint64_t                  g_sent      = 0;
static double                    g_first_recv = -1.0;
static double                    g_last_recv  = -1.0;
static uint64_t                  g_total_bits = 0;

// ─── 应用层：IP/ECMP 路由，应用层直接发目标 IP ──────────────────────────
class DfHost : public Application
{
public:
  static TypeId GetTypeId ()
  {
    static TypeId t = TypeId ("DfHost")
      .SetParent<Application> ()
      .AddConstructor<DfHost> ();
    return t;
  }

  void Setup (uint32_t id, Ptr<Socket> rx, std::map<uint32_t, Address> addr)
  { m_id = id; m_rx = rx; m_addr = std::move (addr); }

  void Send (uint32_t dst, uint32_t bytes, uint64_t id)
  {
    if (m_id == dst) return;
    Ptr<Packet> pkt = Create<Packet> (bytes);
    uint8_t b[12];
    std::memcpy (b,     &id,  8);
    std::memcpy (b + 8, &dst, 4);
    pkt->AddAtEnd (Create<Packet> (b, 12));

    Probe pr; pr.sentNs = Simulator::Now ().GetNanoSeconds (); pr.recvNs = 0;
    g_inflight[id] = pr;
    g_sent++;

    auto it = m_addr.find (dst);
    if (it == m_addr.end ()) return;
    if (!m_tx)
      m_tx = Socket::CreateSocket (GetNode (),
               TypeId::LookupByName ("ns3::UdpSocketFactory"));
    m_tx->SendTo (pkt, 0, it->second);
  }

private:
  void StartApplication () override
  { m_rx->SetRecvCallback (MakeCallback (&DfHost::OnRecv, this)); }
  void StopApplication () override {}

  void OnRecv (Ptr<Socket> s)
  {
    Ptr<Packet> pkt;
    while ((pkt = s->Recv ()))
      {
        if (pkt->GetSize () < 12) continue;
        uint8_t b[12];
        pkt->CreateFragment (pkt->GetSize () - 12, 12)->CopyData (b, 12);
        uint64_t id; std::memcpy (&id, b, 8);

        double nowNs = Simulator::Now ().GetNanoSeconds ();
        if (g_first_recv < 0) g_first_recv = nowNs;
        g_last_recv = nowNs;

        auto it = g_inflight.find (id);
        if (it != g_inflight.end ())
          { it->second.recvNs = nowNs; g_done.push_back (it->second); g_inflight.erase (it); }
      }
  }

  uint32_t m_id {0};
  Ptr<Socket> m_rx, m_tx;
  std::map<uint32_t, Address> m_addr;
};

// ─── main ──────────────────────────────────────────────────────────────────
int main (int argc, char *argv[])
{
  std::string scenario     = "uniform";
  uint32_t    uniformFlows = 200000;
  uint32_t    incastFanin  = 64;
  uint32_t    pktBytes     = 1024;
  // InfiniBand 无损：深队列近似 credit-based 流控，无需 ECN/PFC
  uint32_t    queuePkts    = 256;
  std::string linkRate     = "400Gbps";
  std::string linkDelay    = "100ns";   // IB NDR 铜缆/短光纤典型延迟更低

  CommandLine cmd;
  cmd.AddValue ("scenario",     "uniform|incast|allreduce|bisection", scenario);
  cmd.AddValue ("uniformFlows", "number of random flows",    uniformFlows);
  cmd.AddValue ("incastFanin",  "incast fan-in",             incastFanin);
  cmd.AddValue ("pktBytes",     "payload bytes",             pktBytes);
  cmd.AddValue ("queuePkts",    "IB credit buffer depth (packets)", queuePkts);
  cmd.AddValue ("linkRate",     "link rate",                 linkRate);
  cmd.AddValue ("linkDelay",    "link delay",                linkDelay);
  cmd.Parse (argc, argv);

  // ── ECMP：近似 IB UGAL 自适应路由（在等价最短路径间随机分流）──
  Config::SetDefault ("ns3::Ipv4GlobalRouting::RandomEcmpRouting",
                      BooleanValue (true));

  std::cout << "=== Dragonfly + InfiniBand NDR ===\n"
            << "  a=" << A << " p=" << P << " h=" << H
            << "  g=" << G << " groups  N=" << N << " servers\n"
            << "  Routers=" << A*G << "  Links: "
            << N << "(srv-rtr) + "
            << A*(A-1)/2*G << "(intra) + "
            << G*(G-1)/2 << "(inter) = "
            << N + A*(A-1)/2*G + G*(G-1)/2 << " total\n"
            << "  Link: " << linkRate << " / " << linkDelay
            << "  ECMP: on (approx UGAL)\n\n";

  // ── 创建节点 ──
  NodeContainer servers; servers.Create (N);
  NodeContainer routers; routers.Create (A * G);   // 222 台 Router

  InternetStackHelper inet;
  inet.Install (servers);
  inet.Install (routers);

  // ── 链路模板：IB 无损 = 深 DropTail（credit 缓冲近似）──
  std::ostringstream qs; qs << queuePkts << "p";
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute  ("DataRate", StringValue (linkRate));
  p2p.SetChannelAttribute ("Delay",    StringValue (linkDelay));
  p2p.SetQueue ("ns3::DropTailQueue<Packet>",
                "MaxSize", QueueSizeValue (QueueSize (qs.str ())));

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::FifoQueueDisc",
                        "MaxSize", StringValue (qs.str ()));

  Ipv4AddressHelper ip;
  std::map<uint32_t, Ipv4Address> serverIp;
  uint32_t subnet = 0;

  auto mkLink = [&] (Ptr<Node> u, Ptr<Node> v,
                     Ipv4Address *ipU = nullptr)
  {
    NetDeviceContainer dev = p2p.Install (u, v);
    tch.Install (dev);
    dev.Get (0)->TraceConnectWithoutContext ("MacTx",
      MakeCallback (+[](Ptr<const Packet> p){ g_total_bits += p->GetSize () * 8; }));
    dev.Get (1)->TraceConnectWithoutContext ("MacTx",
      MakeCallback (+[](Ptr<const Packet> p){ g_total_bits += p->GetSize () * 8; }));
    uint32_t base = subnet * 4;
    std::ostringstream b;
    b << "10." << ((base >> 16) & 0xff) << "."
               << ((base >>  8) & 0xff) << "."
               << ( base        & 0xff);
    ip.SetBase (b.str ().c_str (), "255.255.255.252");
    Ipv4InterfaceContainer ic = ip.Assign (dev);
    if (ipU) *ipU = ic.GetAddress (0);
    ++subnet;
    return ic;
  };

  // ── 1. Server ↔ Router 链路 ──
  for (uint32_t g = 0; g < G; ++g)
    for (uint32_t r = 0; r < A; ++r)
      for (uint32_t s = 0; s < P; ++s)
        {
          uint32_t sid = serverIdx (g, r, s);
          Ipv4Address sip;
          mkLink (servers.Get (sid), routers.Get (routerIdx (g, r)), &sip);
          serverIp[sid] = sip;
        }

  // ── 2. Intra-Group 链路（Group 内 Router 全互联）──
  for (uint32_t g = 0; g < G; ++g)
    for (uint32_t r1 = 0; r1 < A; ++r1)
      for (uint32_t r2 = r1 + 1; r2 < A; ++r2)
        mkLink (routers.Get (routerIdx (g, r1)),
                routers.Get (routerIdx (g, r2)));

  // ── 3. Inter-Group Global 链路（平衡分配，每 Router 恰好 H=A 条）──
  //   对于 Group 对 (i, j)（i < j）：
  //     Group i 的 Router ri = (j−i−1) mod A 负责此链路
  //     Group j 的 Router rj = (G−(j−i)−1) mod A 负责此链路
  //   推导：Group i 中 Router r 的第 k 条 Global Link 目标 Group 为
  //         (i + r + 1 + k*A) mod G，共 H=A 条，恰好覆盖所有其他 Group
  {
    std::set<std::pair<uint32_t,uint32_t>> linked;  // 防重复
    uint32_t interLinkCount = 0;
    for (uint32_t gi = 0; gi < G; ++gi)
      for (uint32_t gj = gi + 1; gj < G; ++gj)
        {
          auto key = std::make_pair (gi, gj);
          if (linked.count (key)) continue;
          linked.insert (key);

          uint32_t ri = (gj - gi - 1) % A;
          uint32_t rj = (G - (gj - gi) - 1) % A;
          mkLink (routers.Get (routerIdx (gi, ri)),
                  routers.Get (routerIdx (gj, rj)));
          ++interLinkCount;
        }
    std::cout << "  Inter-group links built: " << interLinkCount
              << "  (expect " << G*(G-1)/2 << ")\n";
  }

  // ── 路由 ──
  std::cout << "  Populating routing tables (1554 nodes)...\n";
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  std::map<uint32_t, Address> addr;
  const uint16_t PORT = 9000;
  for (uint32_t i = 0; i < N; ++i)
    addr[i] = InetSocketAddress (serverIp[i], PORT);

  // ── 安装应用 ──
  std::vector<Ptr<DfHost>> apps (N);
  for (uint32_t i = 0; i < N; ++i)
    {
      Ptr<Socket> rx = Socket::CreateSocket (servers.Get (i),
                          TypeId::LookupByName ("ns3::UdpSocketFactory"));
      rx->Bind (InetSocketAddress (Ipv4Address::GetAny (), PORT));
      Ptr<DfHost> a = CreateObject<DfHost> ();
      a->Setup (i, rx, addr);
      servers.Get (i)->AddApplication (a);
      a->SetStartTime (Seconds (0.0));
      a->SetStopTime  (Seconds (30.0));
      apps[i] = a;
    }

  // ── 调度流量 ──
  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable> ();
  uint64_t pid = 1;
  DataRate dr (linkRate);
  double serSec = (pktBytes * 8.0) / (double) dr.GetBitRate ();

  if (scenario == "incast")
    {
      // 多对一：incastFanin 台服务器打向 server 0
      // 在 Dragonfly 中，来源分散于多个 Group → 测试 Global Link 聚合
      uint32_t dst  = 0;
      double   base = 1.0;
      uint32_t cnt  = 0;
      for (uint32_t s = 1; s < N && cnt < incastFanin; ++s, ++cnt)
        for (uint32_t m = 0; m < 64; ++m)
          {
            double when = base + (cnt * 64 + m) * serSec;
            Simulator::Schedule (Seconds (when), &DfHost::Send, apps[s],
                                 dst, pktBytes, pid++);
          }
      std::cout << "  Scenario: incast fan-in="
                << std::min (incastFanin, N - 1) << " -> server 0\n";
    }
  else if (scenario == "allreduce")
    {
      // Ring AllReduce：每台服务器向"下一台"发 64 包
      // Dragonfly 中相邻 server 可能在同 Group（Intra 路径）或跨 Group（Global 路径）
      double base = 1.0;
      for (uint32_t s = 0; s < N; ++s)
        {
          uint32_t dst = (s + 1) % N;
          for (uint32_t m = 0; m < 64; ++m)
            Simulator::Schedule (Seconds (base + m * serSec),
                                 &DfHost::Send, apps[s], dst, pktBytes, pid++);
        }
      std::cout << "  Scenario: allreduce ring (N=" << N << ")\n";
    }
  else if (scenario == "bisection")
    {
      // 平分带宽：前 G/2 个 Group 的服务器打后 G/2 个 Group 的服务器
      // 所有流量必须经过 Global Link，是 Dragonfly 最大压力测试
      double   base = 1.0;
      uint32_t cnt  = 0;
      uint32_t halfN = N / 2;
      for (uint32_t s = 0; s < halfN; ++s)
        for (uint32_t m = 0; m < 64; ++m, ++cnt)
          {
            uint32_t dst = halfN + s;
            Simulator::Schedule (Seconds (base + cnt * serSec),
                                 &DfHost::Send, apps[s], dst, pktBytes, pid++);
          }
      std::cout << "  Scenario: bisection (first " << G/2 << " groups -> last "
                << G - G/2 << " groups)\n";
    }
  else  // uniform
    {
      double when = 1.0;
      for (uint32_t k = 0; k < uniformFlows; ++k)
        {
          uint32_t s = rng->GetInteger (0, N - 1);
          uint32_t d = rng->GetInteger (0, N - 1);
          if (s == d) continue;
          Simulator::Schedule (Seconds (when), &DfHost::Send, apps[s],
                               d, pktBytes, pid++);
          when += 5e-7;
        }
      std::cout << "  Scenario: uniform random (" << uniformFlows << " flows)\n";
    }

  // ── 运行 ──
  double simDuration = 1.5;
  Simulator::Stop (Seconds (simDuration));
  std::cout << "  Running simulation...\n\n";
  Simulator::Run ();
  Simulator::Destroy ();

  // ── 统计 ──
  uint64_t delivered = g_done.size ();
  uint64_t dropped   = (g_sent >= delivered) ? (g_sent - delivered) : 0;
  double rxDurSec = (g_last_recv > g_first_recv && g_first_recv > 0)
                    ? (g_last_recv - g_first_recv) / 1e9 : 0.0;
  double tputGbps = rxDurSec > 0
                    ? (delivered * pktBytes * 8.0) / (rxDurSec * 1e9) : 0.0;

  // ── 延迟分布 ──
  std::vector<double> lats;
  lats.reserve (g_done.size ());
  for (auto &p : g_done)
    if (p.recvNs > 0) lats.push_back (p.recvNs - p.sentNs);
  std::sort (lats.begin (), lats.end ());
  double meanUs = 0, p50Us = 0, p99Us = 0, maxUs = 0;
  if (!lats.empty ())
    {
      double sum = 0; for (double v : lats) sum += v;
      meanUs = sum / lats.size () / 1000.0;
      p50Us  = lats[lats.size () * 50 / 100] / 1000.0;
      p99Us  = lats[lats.size () * 99 / 100] / 1000.0;
      maxUs  = lats.back () / 1000.0;
    }

  // ── 能耗模型 ──
  // IB 交换机（HCA + Router）功耗参考 NVIDIA QM9700 规格：约 350W
  uint32_t nRouters   = A * G;
  double P_srv_static = 2000.0;
  double P_srv_full   = 8000.0;
  double P_rtr_static = 350.0;   // W（IB NDR Router，含 HCA）
  double E_dyn_bit    = 10e-12;

  double staticEnergy  = (N * P_srv_static + nRouters * P_rtr_static) * simDuration;
  double computeEnergy = rxDurSec > 0 ? N * (P_srv_full - P_srv_static) * rxDurSec : 0;
  double netEnergy     = g_total_bits * E_dyn_bit;
  double totalEnergy   = staticEnergy + computeEnergy + netEnergy;
  double avgPower      = totalEnergy / simDuration;

  // ── 输出 ──
  std::cout << "=== Results: scenario=" << scenario
            << "  a=" << A << " g=" << G << " N=" << N << " ===\n";
  std::cout << "sent=" << g_sent << "  delivered=" << delivered
            << "  dropped=" << dropped
            << " (" << (g_sent ? 100.0 * dropped / g_sent : 0.0) << "%)\n";
  std::cout << "-------------------------------------------\n";
  std::cout << "吞吐量 (Throughput)     : " << tputGbps << " Gbps\n";
  std::cout << "接收有效时长 (Duration)  : " << rxDurSec  << " s\n";
  std::cout << "-------------------------------------------\n";
  std::cout << "延迟分布 (Latency)\n";
  std::cout << "  mean=" << meanUs << " µs  p50=" << p50Us
            << " µs  p99=" << p99Us << " µs  max=" << maxUs << " µs\n";
  std::cout << "-------------------------------------------\n";
  std::cout << "全网总能耗 (Total Energy): " << totalEnergy << " J\n";
  std::cout << "  ├─ 服务器静态         : "
            << N * P_srv_static * simDuration << " J  (" << N << " × 2000W)\n";
  std::cout << "  ├─ 服务器计算增量     : " << computeEnergy << " J  (满载 8000W)\n";
  std::cout << "  ├─ IB Router/HCA 静态 : "
            << nRouters * P_rtr_static * simDuration
            << " J  (" << nRouters << " × 350W)\n";
  std::cout << "  └─ 链路动态传输       : " << netEnergy << " J\n";
  std::cout << "全网平均总功耗 (Avg Power): " << avgPower << " W\n";
  std::cout << "-------------------------------------------\n";
  // Dragonfly 特有指标：Global Link 利用率
  std::cout << "拓扑参数\n";
  std::cout << "  直径 (Diameter)  : 3 Router hops（跨 Group）\n";
  std::cout << "  二分带宽 (Bisection BW): " << G*(G-1)/2 << " × 400Gbps Global Links\n";
  std::cout << "  节点度 (Router度): "
            << (P + (A-1) + H) << " 端口"
            << " (" << P << " server + " << A-1 << " intra + " << H << " global)\n";
  std::cout << "-------------------------------------------\n";

  // ── CSV ──
  std::ofstream csv ("dragonfly_ib_result.csv");
  csv << "scenario,a,p,h,g,N,routers,sent,delivered,dropped,"
         "throughput_Gbps,mean_us,p99_us,total_energy_J,avg_power_W\n";
  csv << scenario << "," << A << "," << P << "," << H << "," << G << "," << N
      << "," << nRouters << ","
      << g_sent << "," << delivered << "," << dropped << ","
      << tputGbps << "," << meanUs << "," << p99Us << ","
      << totalEnergy << "," << avgPower << "\n";
  std::cout << "wrote dragonfly_ib_result.csv\n";
  return 0;
}
