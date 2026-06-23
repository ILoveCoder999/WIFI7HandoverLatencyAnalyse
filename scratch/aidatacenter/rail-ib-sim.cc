/*
 * rail-ib-sim.cc
 *
 * Rail-Optimized InfiniBand NDR 仿真
 * ────────────────────────────────────────────────────────────
 * 架构概述（参考 NVIDIA DGX SuperPOD / H100 NVL72）
 * ─────────────────────────────────────────────────
 *   · 每台物理服务器有 R 块独立 HCA（一块对应一块 GPU）
 *   · R 条完全独立的 IB NDR Rail，每条 Rail = 一个 2-tier Leaf-Spine Clos
 *   · GPU 间通信规则：
 *       - 同 Rail（相同 GPU 序号，不同服务器）→ 走 Rail 网络
 *       - 跨 Rail（同服务器不同 GPU）          → 走 NVLink（不经过网络，不模拟）
 *   · 核心优势：
 *       - AllReduce 分 R 条 Rail 并行执行，每条携带 1/R 数据
 *       - 各 Rail 完全隔离，无跨 Rail 竞争
 *       - 总有效带宽 = R × 单 Rail 带宽
 *
 * 仿真参数（N=1332，对齐 3D Mesh 1331）
 * ───────────────────────────────────────
 *   R = 4 条 Rail（代表每服务器 4 张 GPU/HCA）
 *   N_PER_RAIL = 333 endpoints（4×333 = 1332 总 GPU 节点）
 *   每条 Rail：K=32 Leaf-Spine
 *     · serversPerLeaf = 16，nLeaf = 21（21×16≥333，最后 1 台 Leaf 下挂 13 台）
 *     · nSpine = 16
 *     · 过订阅比 = 21/16 = 1.3x（实际部署中常见，单 Rail 独立不需完全无阻塞）
 *   链路：400 Gbps，100 ns（IB NDR 铜缆/DAC 典型延迟）
 *
 * InfiniBand 无损近似
 * ─────────────────────
 *   · IB 使用硬件 credit-based 流控（VL-based credits），天然无丢包
 *   · ns3 近似：深 DropTail 队列（queuePkts=256），不使用 ECN/PFC
 *   · IB 链路延迟更低（100ns），与以太网（200ns）区分
 *
 * 流量场景
 * ──────────
 *   uniform   : 各 Rail 内独立均匀随机打流
 *   incast    : 各 Rail 内多对一（打向本 Rail 第 0 号节点）
 *   allreduce : 各 Rail 内独立 Ring（模拟 ring-allreduce reduce-scatter 步）
 *               ← Rail-Optimized 最核心优势：R=4 条 Ring 完全并行，无竞争
 *   bisection : 各 Rail 内前半打后半（每条 Rail 独立平分带宽测试）
 *
 * 输出：
 *   · 每条 Rail 独立统计 + 全系统聚合（×R 估算）
 *   · 全系统等效吞吐 = R × 单 Rail 吞吐（各 Rail 独立无竞争时成立）
 */

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/ipv4-global-routing-helper.h"

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("RailIbSim");

// ─── 拓扑常量 ──────────────────────────────────────────────────────────────
static const uint32_t R           = 4;    // Rail 数量（= 每服务器 GPU/HCA 数）
static const uint32_t N_PER_RAIL  = 333;  // 每条 Rail 的 GPU 端点数
static const uint32_t N           = R * N_PER_RAIL;  // 总 GPU 节点 = 1332

// K=32 Leaf-Spine 参数：略微过订阅（1.3x），单 Rail 隔离时完全可接受
static const uint32_t K_RAIL      = 32;
static const uint32_t SRV_PER_LEAF= K_RAIL / 2;   // = 16
static const uint32_t N_LEAF      = (N_PER_RAIL + SRV_PER_LEAF - 1) / SRV_PER_LEAF; // = 21 (ceiling)
static const uint32_t N_SPINE     = K_RAIL / 2;   // = 16

// ─── 全局统计（跨所有 Rail 聚合）──────────────────────────────────────────
struct Probe { double sentNs; double recvNs; uint32_t rail; };
static std::map<uint64_t, Probe> g_inflight;
static std::vector<Probe>        g_done;
static uint64_t                  g_sent      = 0;
// 按 Rail 分组的接收时间（用于计算各 Rail 独立吞吐）
static double g_first_recv[R], g_last_recv[R];
static uint64_t g_total_bits = 0;

// ─── 应用层 ─────────────────────────────────────────────────────────────────
class RailHost : public Application
{
public:
  static TypeId GetTypeId ()
  {
    static TypeId t = TypeId ("RailHost")
      .SetParent<Application> ()
      .AddConstructor<RailHost> ();
    return t;
  }

  void Setup (uint32_t id, uint32_t rail, Ptr<Socket> rx,
              std::map<uint32_t, Address> addr)
  { m_id = id; m_rail = rail; m_rx = rx; m_addr = std::move (addr); }

  void Send (uint32_t dst, uint32_t bytes, uint64_t id)
  {
    if (m_id == dst) return;
    Ptr<Packet> pkt = Create<Packet> (bytes);
    uint8_t b[12];
    std::memcpy (b,     &id,  8);
    std::memcpy (b + 8, &dst, 4);
    pkt->AddAtEnd (Create<Packet> (b, 12));

    Probe pr; pr.sentNs = Simulator::Now ().GetNanoSeconds ();
    pr.recvNs = 0; pr.rail = m_rail;
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
  { m_rx->SetRecvCallback (MakeCallback (&RailHost::OnRecv, this)); }
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
        if (g_first_recv[m_rail] < 0) g_first_recv[m_rail] = nowNs;
        g_last_recv[m_rail] = nowNs;

        auto it = g_inflight.find (id);
        if (it != g_inflight.end ())
          { it->second.recvNs = nowNs; g_done.push_back (it->second); g_inflight.erase (it); }
      }
  }

  uint32_t m_id {0}, m_rail {0};
  Ptr<Socket> m_rx, m_tx;
  std::map<uint32_t, Address> m_addr;
};

// ─── main ──────────────────────────────────────────────────────────────────
int main (int argc, char *argv[])
{
  std::string scenario     = "allreduce";  // 默认 allreduce：Rail-Optimized 最核心优势
  uint32_t    uniformFlows = 200000;
  uint32_t    incastFanin  = 64;
  uint32_t    pktBytes     = 1024;
  uint32_t    queuePkts    = 256;           // IB 深队列，近似 credit-based 无损
  std::string linkRate     = "400Gbps";
  std::string linkDelay    = "100ns";       // IB NDR 低延迟

  CommandLine cmd;
  cmd.AddValue ("scenario",     "uniform|incast|allreduce|bisection", scenario);
  cmd.AddValue ("uniformFlows", "random flows per rail",    uniformFlows);
  cmd.AddValue ("incastFanin",  "incast fan-in per rail",   incastFanin);
  cmd.AddValue ("pktBytes",     "payload bytes",            pktBytes);
  cmd.AddValue ("queuePkts",    "IB credit buffer depth",   queuePkts);
  cmd.AddValue ("linkRate",     "link rate",                linkRate);
  cmd.AddValue ("linkDelay",    "link delay",               linkDelay);
  cmd.Parse (argc, argv);

  for (uint32_t r = 0; r < R; ++r)
    { g_first_recv[r] = -1.0; g_last_recv[r] = -1.0; }

  // ECMP：多条等价路径（多台 Spine）随机分流，近似 IB UGAL
  Config::SetDefault ("ns3::Ipv4GlobalRouting::RandomEcmpRouting",
                      BooleanValue (true));

  std::cout << "=== Rail-Optimized InfiniBand NDR ===\n"
            << "  R=" << R << " rails  N_per_rail=" << N_PER_RAIL
            << "  Total N=" << N << "\n"
            << "  Per-rail topology: K=" << K_RAIL
            << "  nLeaf=" << N_LEAF << "  nSpine=" << N_SPINE
            << "  oversubscription=" << (double)N_LEAF/N_SPINE << "x\n"
            << "  Link: " << linkRate << " / " << linkDelay
            << "  (IB credit-based lossless, deep queue=" << queuePkts << "p)\n\n";

  // ── 创建节点 ──
  NodeContainer servers; servers.Create (N);
  // 交换机：每条 Rail 有 N_LEAF 台 Leaf + N_SPINE 台 Spine
  NodeContainer leaves[R], spines[R];
  for (uint32_t r = 0; r < R; ++r)
    { leaves[r].Create (N_LEAF); spines[r].Create (N_SPINE); }

  InternetStackHelper inet;
  inet.Install (servers);
  for (uint32_t r = 0; r < R; ++r)
    { inet.Install (leaves[r]); inet.Install (spines[r]); }

  // ── 链路模板（IB 无损：深 DropTail）──
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
  // addr[server_global_id] → InetSocketAddress（仅同 Rail 内有效）
  std::map<uint32_t, Address> addr;
  std::map<uint32_t, Ipv4Address> serverIp;
  uint32_t subnet = 0;
  const uint16_t PORT = 9000;

  auto mkLink = [&] (Ptr<Node> u, Ptr<Node> v, Ipv4Address *ipU = nullptr)
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

  // ── 建立 4 条独立 Rail 网络 ──
  for (uint32_t r = 0; r < R; ++r)
    {
      uint32_t srv_base = r * N_PER_RAIL;  // Rail r 的服务器全局 ID 起点

      // 1. Server → Leaf（每台 Leaf 下接 SRV_PER_LEAF 台服务器）
      for (uint32_t s = 0; s < N_PER_RAIL; ++s)
        {
          uint32_t  leaf_id = s / SRV_PER_LEAF;   // 此服务器属于哪台 Leaf
          uint32_t  sid     = srv_base + s;
          Ipv4Address sip;
          mkLink (servers.Get (sid), leaves[r].Get (leaf_id), &sip);
          serverIp[sid] = sip;
        }

      // 2. Leaf → Spine（全互联，每对 Leaf-Spine 一条链路）
      for (uint32_t l = 0; l < N_LEAF; ++l)
        for (uint32_t sp = 0; sp < N_SPINE; ++sp)
          mkLink (leaves[r].Get (l), spines[r].Get (sp));
    }

  std::cout << "  Links built: " << subnet << " total  ("
            << R << " rails × " << subnet/R << " per rail)\n";

  // ── 路由 ──
  std::cout << "  Populating routing tables (" << N + R*(N_LEAF+N_SPINE)
            << " nodes)...\n";
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // 地址表（仅同 Rail 内有路由，跨 Rail SendTo 会静默失败——符合 IB Rail 隔离语义）
  for (uint32_t i = 0; i < N; ++i)
    addr[i] = InetSocketAddress (serverIp[i], PORT);

  // ── 安装应用 ──
  std::vector<Ptr<RailHost>> apps (N);
  for (uint32_t i = 0; i < N; ++i)
    {
      uint32_t rail = i / N_PER_RAIL;
      Ptr<Socket> rx = Socket::CreateSocket (servers.Get (i),
                          TypeId::LookupByName ("ns3::UdpSocketFactory"));
      rx->Bind (InetSocketAddress (Ipv4Address::GetAny (), PORT));
      Ptr<RailHost> a = CreateObject<RailHost> ();
      a->Setup (i, rail, rx, addr);
      servers.Get (i)->AddApplication (a);
      a->SetStartTime (Seconds (0.0));
      a->SetStopTime  (Seconds (30.0));
      apps[i] = a;
    }

  // ── 调度流量（只在同 Rail 内调度）──
  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable> ();
  uint64_t pid = 1;
  DataRate dr (linkRate);
  double serSec = (pktBytes * 8.0) / (double) dr.GetBitRate ();

  if (scenario == "allreduce")
    {
      // Ring AllReduce：每条 Rail 独立运行 ring，共 R 条并行环
      // Rail r 中：r*N_PER_RAIL → r*N_PER_RAIL+1 → ... → (r+1)*N_PER_RAIL-1 → r*N_PER_RAIL
      // 这是 Rail-Optimized 的核心优势：R 条环完全隔离，没有跨 Rail 竞争
      double base = 1.0;
      for (uint32_t r = 0; r < R; ++r)
        {
          uint32_t base_id = r * N_PER_RAIL;
          for (uint32_t s = 0; s < N_PER_RAIL; ++s)
            {
              uint32_t src = base_id + s;
              uint32_t dst = base_id + (s + 1) % N_PER_RAIL;
              for (uint32_t m = 0; m < 64; ++m)
                Simulator::Schedule (Seconds (base + m * serSec),
                                     &RailHost::Send, apps[src], dst, pktBytes, pid++);
            }
        }
      std::cout << "  Scenario: allreduce ring × " << R << " parallel rails"
                << " (N_per_ring=" << N_PER_RAIL << ")\n";
    }
  else if (scenario == "incast")
    {
      // 各 Rail 内独立 incast，目标：本 Rail 第 0 号节点
      double base = 1.0;
      for (uint32_t r = 0; r < R; ++r)
        {
          uint32_t base_id = r * N_PER_RAIL;
          uint32_t dst     = base_id;  // Rail r 的第 0 个节点
          uint32_t cnt = 0;
          for (uint32_t s = 1; s < N_PER_RAIL && cnt < incastFanin; ++s, ++cnt)
            for (uint32_t m = 0; m < 64; ++m)
              {
                double when = base + (r * incastFanin * 64 + cnt * 64 + m) * serSec;
                Simulator::Schedule (Seconds (when), &RailHost::Send,
                                     apps[base_id + s], dst, pktBytes, pid++);
              }
        }
      std::cout << "  Scenario: incast fan-in=" << std::min(incastFanin, N_PER_RAIL-1)
                << " per rail × " << R << " rails\n";
    }
  else if (scenario == "bisection")
    {
      // 各 Rail 内平分带宽：前半打后半
      double base = 1.0;
      for (uint32_t r = 0; r < R; ++r)
        {
          uint32_t base_id = r * N_PER_RAIL;
          uint32_t cnt = 0;
          for (uint32_t s = 0; s < N_PER_RAIL / 2; ++s)
            for (uint32_t m = 0; m < 64; ++m, ++cnt)
              {
                uint32_t src = base_id + s;
                uint32_t dst = base_id + N_PER_RAIL / 2 + s;
                Simulator::Schedule (Seconds (base + cnt * serSec),
                                     &RailHost::Send, apps[src], dst, pktBytes, pid++);
              }
        }
      std::cout << "  Scenario: bisection (within each rail) × " << R << " rails\n";
    }
  else  // uniform
    {
      // 各 Rail 内均匀随机（不跨 Rail）
      double when = 1.0;
      uint32_t flows_per_rail = uniformFlows / R;
      for (uint32_t r = 0; r < R; ++r)
        {
          uint32_t base_id = r * N_PER_RAIL;
          for (uint32_t k = 0; k < flows_per_rail; ++k)
            {
              uint32_t s = base_id + rng->GetInteger (0, N_PER_RAIL - 1);
              uint32_t d = base_id + rng->GetInteger (0, N_PER_RAIL - 1);
              if (s == d) continue;
              Simulator::Schedule (Seconds (when), &RailHost::Send, apps[s],
                                   d, pktBytes, pid++);
              when += 5e-7;
            }
        }
      std::cout << "  Scenario: uniform random × " << R << " rails\n";
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

  // 各 Rail 独立吞吐
  double railTput[R] = {};
  for (uint32_t r = 0; r < R; ++r)
    {
      if (g_last_recv[r] > g_first_recv[r] && g_first_recv[r] > 0)
        {
          double dur = (g_last_recv[r] - g_first_recv[r]) / 1e9;
          // 统计本 Rail 收到的包数
          uint64_t rail_deliv = 0;
          for (auto &p : g_done)
            if (p.rail == r && p.recvNs > 0) ++rail_deliv;
          railTput[r] = (rail_deliv * pktBytes * 8.0) / (dur * 1e9);
        }
    }
  double totalTput = 0;
  for (uint32_t r = 0; r < R; ++r) totalTput += railTput[r];

  // 延迟分布（跨所有 Rail）
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

  // 能耗（参考 NVIDIA QM9700 400G IB：约 350W/switch）
  uint32_t nSwitches    = R * (N_LEAF + N_SPINE);
  double P_srv_static   = 2000.0;
  double P_srv_full     = 8000.0;
  double P_sw_ib        = 350.0;    // W（IB NDR 交换机）
  double E_dyn_bit      = 10e-12;

  // 取各 Rail 中最长有效接收窗口作为计算能耗的时长
  double rxDurMax = 0;
  for (uint32_t r = 0; r < R; ++r)
    if (g_last_recv[r] > g_first_recv[r] && g_first_recv[r] > 0)
      rxDurMax = std::max (rxDurMax, (g_last_recv[r] - g_first_recv[r]) / 1e9);

  double staticEnergy  = (N * P_srv_static + nSwitches * P_sw_ib) * simDuration;
  double computeEnergy = rxDurMax > 0 ? N * (P_srv_full - P_srv_static) * rxDurMax : 0;
  double netEnergy     = g_total_bits * E_dyn_bit;
  double totalEnergy   = staticEnergy + computeEnergy + netEnergy;
  double avgPower      = totalEnergy / simDuration;

  // ── 输出 ──
  std::cout << "=== Results: scenario=" << scenario
            << "  R=" << R << "  N=" << N << " ===\n";
  std::cout << "sent=" << g_sent << "  delivered=" << delivered
            << "  dropped=" << dropped
            << " (" << (g_sent ? 100.0*dropped/g_sent : 0.0) << "%)\n";
  std::cout << "-------------------------------------------\n";
  std::cout << "各 Rail 吞吐：\n";
  for (uint32_t r = 0; r < R; ++r)
    printf("  Rail %u: %.3f Gbps\n", r, railTput[r]);
  std::cout << "全系统聚合吞吐 (R Rails 独立运行): " << totalTput << " Gbps\n";
  std::cout << "-------------------------------------------\n";
  std::cout << "延迟分布 (Latency)\n";
  std::cout << "  mean=" << meanUs << " µs  p50=" << p50Us
            << " µs  p99=" << p99Us << " µs  max=" << maxUs << " µs\n";
  std::cout << "-------------------------------------------\n";
  std::cout << "全网总能耗 (Total Energy): " << totalEnergy << " J\n";
  std::cout << "  ├─ 服务器静态         : "
            << N * P_srv_static * simDuration << " J  (" << N << " × 2000W)\n";
  std::cout << "  ├─ 服务器计算增量     : " << computeEnergy << " J\n";
  std::cout << "  ├─ IB 交换机静态      : "
            << nSwitches * P_sw_ib * simDuration
            << " J  (" << nSwitches << " × 350W  "
            << R << " rails × " << N_LEAF+N_SPINE << " sw/rail)\n";
  std::cout << "  └─ 链路动态传输       : " << netEnergy << " J\n";
  std::cout << "全网平均总功耗 (Avg Power): " << avgPower << " W\n";
  std::cout << "-------------------------------------------\n";
  std::cout << "拓扑指标\n";
  std::cout << "  每服务器总带宽 : R × 400G = " << R*400 << " Gbps\n";
  std::cout << "  Rail 隔离性   : 跨 Rail 流量走 NVLink，不竞争网络带宽\n";
  std::cout << "  Bisection BW  : " << R << " rails × "
            << N_LEAF*N_SPINE << " links × 400G = "
            << (double)R*N_LEAF*N_SPINE*400 << " Gbps\n";
  std::cout << "-------------------------------------------\n";

  // CSV
  std::ofstream csv ("rail_ib_result.csv");
  csv << "scenario,R,N_per_rail,N_total,switches,"
         "total_throughput_Gbps,mean_us,p99_us,"
         "sent,delivered,dropped,total_energy_J,avg_power_W\n";
  csv << scenario << "," << R << "," << N_PER_RAIL << "," << N << ","
      << nSwitches << "," << totalTput << "," << meanUs << "," << p99Us << ","
      << g_sent << "," << delivered << "," << dropped << ","
      << totalEnergy << "," << avgPower << "\n";
  std::cout << "wrote rail_ib_result.csv\n";
  return 0;
}
