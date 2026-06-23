/*
 * rng-sim.cc
 *
 * 复现论文：RNG: Flat Datacenter Networks at Scale (SIGCOMM 2026)
 * ─────────────────────────────────────────────────────────────────
 * 目标：重现 Figure 13 — 过订阅比 vs 活跃比例（clique/hubs/matching）
 *       RNG vs Fat-Tree 相同规模下的吞吐对比
 *
 * ═══════════════════════════════════════════════════════════════
 * 拓扑（论文 §9.3）
 * ═══════════════════════════════════════════════════════════════
 *  RNG (--topology=rng)：
 *    n=100 节点，d=16 度（对应论文 n=1000, d=64 缩小 10/4x）
 *    随机正则图（RRG），configuration model 生成
 *
 *  Fat-Tree (--topology=fattree)：
 *    100 Leaf 节点 + 16 Spine 节点
 *    每 Leaf 有 d=16 条上行链路连接全部 16 台 Spine（完全无阻塞）
 *    与 RNG 规模对齐：100 端点，每端点 16 条链路
 *
 * ═══════════════════════════════════════════════════════════════
 * 路由（论文 §5）
 * ═══════════════════════════════════════════════════════════════
 *  两种拓扑均使用 GlobalRouting + RandomEcmpRouting。
 *
 *  论文中 Spraypoint 路由的核心是：
 *    1. 源节点向随机邻居喷射（Spray = ECMP at source）
 *    2. 后续节点沿"路标层"（Waypoint levels）转发
 *  这两步在 ns3 中均可由 RandomEcmpRouting 近似：
 *    - 在多条等价最短路径间随机选择 ≈ 喷射+路标指向
 *    - RNG 拓扑的丰富路径多样性（多边不相交路径）自然地
 *      使 ECMP 效果逼近 Spraypoint（尤其在 n=100 规模下）
 *  Fat-Tree 的 ECMP 等价路径集合远小于 RNG，
 *  这正是 Figure 13 中 RNG 优于 Fat-Tree 的根本原因。
 *
 * ═══════════════════════════════════════════════════════════════
 * 流量模式（论文 §9.3，对应 Figure 13）
 * ═══════════════════════════════════════════════════════════════
 *  clique(f)  : ceil(f*N) 节点两两全互联，每对发 BURST_PKTS 包
 *               → 测试网络处理全对全集合通信的能力
 *  hubs(f)    : ceil(f*N) 个 Hub，所有其他节点向所有 Hub 发包
 *               → 测试 Hub 处聚合拥塞（参数服务器场景）
 *  matching(f): ceil(f*N/2) 对随机配对，每对双向发包
 *               → 测试一对一随机流量下的平分带宽
 *
 *  所有包在 t=1.0s 同时注入（突发）→ 队列溢出 → 丢包率反映过订阅
 *
 * ═══════════════════════════════════════════════════════════════
 * 过订阅近似（对应 Figure 13 纵轴）
 * ═══════════════════════════════════════════════════════════════
 *  论文用 LP 求最大可送达流量分数（过订阅比 r）。
 *  本仿真用：  oversub ≈ 1 / delivery_rate
 *    其中 delivery_rate = delivered/sent ∈ (0,1]
 *  解读：delivery_rate=0.33 对应过订阅比≈3:1（图中 y 轴约 3.0）
 *
 * ═══════════════════════════════════════════════════════════════
 * 使用方法（复现 Figure 13）
 * ═══════════════════════════════════════════════════════════════
 *  步骤1：运行两次，分别产生 RNG 和 Fat-Tree 数据
 *    ./rng-sim --topology=rng --scanAll=1
 *    ./rng-sim --topology=fattree --scanAll=1
 *
 *  步骤2：两次运行各输出 rng_fig13.csv，
 *    对比 delivery_rate 或 oversub 列即可重现 Figure 13
 *
 *  单次运行示例：
 *    ./rng-sim --topology=rng --pattern=clique --f=0.3
 *    ./rng-sim --topology=fattree --pattern=hubs --f=0.5
 */

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>
#include <deque>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/ipv4-global-routing-helper.h"

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("RngSim");

// ─── 拓扑参数 ──────────────────────────────────────────────────────────────
static const uint32_t N_RNG    = 100;  // RNG 节点数（论文 n=1000 ÷ 10）
static const uint32_t D_RNG    = 16;   // RNG 节点度（论文 d=64 ÷ 4）
static const uint32_t N_LEAF   = N_RNG;
static const uint32_t N_SPINE  = D_RNG; // Spine 数 = Leaf 上行链路数

// ─── 流量参数 ──────────────────────────────────────────────────────────────
static const uint32_t BURST_PKTS = 100;   // 每条流突发包数（小队列下快速产生拥塞）
static const uint32_t PKT_BYTES  = 1024;
static const uint16_t DATA_PORT  = 9000;

// ─── 全局统计 ──────────────────────────────────────────────────────────────
// 使用唯一名称避免与 ns3 内部符号冲突
struct RngProbe { double sentNs; double recvNs; };
static std::map<uint64_t, RngProbe> g_inflight;
static std::vector<RngProbe>        g_done;
static uint64_t                     g_sent      = 0;
static double                       g_first_recv = -1.0;
static double                       g_last_recv  = -1.0;
static uint64_t                     g_total_bits = 0;

// ─── 通用 Host（IP 路由，不需要应用层转发）─────────────────────────────────
class NetHost : public Application
{
public:
  static TypeId GetTypeId ()
  {
    static TypeId t = TypeId ("NetHost")
      .SetParent<Application> ()
      .AddConstructor<NetHost> ();
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

    RngProbe pr; pr.sentNs = Simulator::Now ().GetNanoSeconds (); pr.recvNs = 0;
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
  { m_rx->SetRecvCallback (MakeCallback (&NetHost::OnRecv, this)); }
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

// ─── 生成 RRG（configuration model）- 修复死循环问题 ──────────────────────
std::vector<std::pair<uint32_t,uint32_t>>
GenerateRRG (uint32_t n, uint32_t d, Ptr<UniformRandomVariable> rng)
{
  std::vector<std::pair<uint32_t,uint32_t>> edges;
  int attempts = 0;
  const int MAX_ATTEMPTS = 200;
  
  while (attempts < MAX_ATTEMPTS)
    {
      ++attempts;
      std::vector<uint32_t> stubs;
      stubs.reserve (n * d);
      for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < d; ++j)
          stubs.push_back (i);

      // Fisher-Yates 洗牌
      for (int64_t i = (int64_t)stubs.size()-1; i > 0; --i)
        { 
          uint32_t j = rng->GetInteger(0, (uint32_t)i); 
          std::swap(stubs[i], stubs[j]); 
        }

      bool valid = true;
      std::set<std::pair<uint32_t,uint32_t>> seen;
      edges.clear ();
      
      for (size_t i = 0; i+1 < stubs.size(); i += 2)
        {
          uint32_t u = stubs[i], v = stubs[i+1];
          if (u == v) { valid = false; break; }
          auto key = std::make_pair (std::min(u,v), std::max(u,v));
          if (seen.count(key)) { valid = false; break; }
          seen.insert(key);
          edges.push_back ({u, v});
        }
        
      if (valid)
        { 
          std::cout << "  RRG generated after " << attempts << " attempt(s)\n"; 
          return edges;
        }
    }
  
  // 备用方案：构建一个近似的 d-正则图（环 + 随机补边）
  std::cout << "  RRG generation failed after " << MAX_ATTEMPTS 
            << " attempts, using fallback method\n";
  edges.clear();
  
  // 先构建一个环（度数2）
  for (uint32_t i = 0; i < n; ++i)
    edges.push_back ({i, (i+1) % n});
  
  // 记录当前每个节点的度数
  std::vector<uint32_t> deg(n, 0);
  for (auto &e : edges) {
    deg[e.first]++;
    deg[e.second]++;
  }
  
  // 添加随机边直到达到度数 d
  uint32_t max_extra_edges = 10000;
  uint32_t extra_edges = 0;
  
  while (extra_edges < max_extra_edges) {
    bool all_done = true;
    for (uint32_t i = 0; i < n; ++i) {
      if (deg[i] < d) {
        all_done = false;
        // 找一个度数不足的节点配对
        for (uint32_t j = 0; j < n; ++j) {
          if (i != j && deg[j] < d) {
            auto key = std::make_pair(std::min(i,j), std::max(i,j));
            bool already = false;
            for (auto &e : edges) {
              if ((e.first == key.first && e.second == key.second) ||
                  (e.first == key.second && e.second == key.first)) {
                already = true;
                break;
              }
            }
            if (!already) {
              edges.push_back({i, j});
              deg[i]++;
              deg[j]++;
              extra_edges++;
              break;
            }
          }
        }
        break;
      }
    }
    if (all_done) break;
  }
  
  std::cout << "  Fallback graph generated with " << edges.size() << " edges\n";
  std::cout << "  Node degrees: min=" << *std::min_element(deg.begin(), deg.end())
            << " max=" << *std::max_element(deg.begin(), deg.end()) << "\n";
  return edges;
}

// ─── 运行单次仿真，返回统计数据 ───────────────────────────────────────────
struct SimResult
{
  uint64_t sent, delivered, dropped;
  double   deliveryRate;
  double   oversubApprox;
  double   throughputGbps;
  double   meanUs, p99Us;
};

SimResult RunSim (const std::string &topology,
                  const std::string &pattern,
                  double f,
                  uint32_t queuePkts,
                  const std::string &linkRate,
                  const std::string &linkDelay)
{
  // 清空全局状态
  g_inflight.clear (); g_done.clear ();
  g_sent = 0; g_first_recv = -1.0; g_last_recv = -1.0; g_total_bits = 0;

  bool isRng = (topology == "rng");
  uint32_t N_ep = isRng ? N_RNG : N_LEAF;

  Config::SetDefault ("ns3::Ipv4GlobalRouting::RandomEcmpRouting",
                      BooleanValue (true));

  // ── 节点 ──
  NodeContainer endpoints; endpoints.Create (N_ep);
  NodeContainer spines;
  if (!isRng) spines.Create (N_SPINE);

  InternetStackHelper inet;
  inet.Install (endpoints);
  if (!isRng) inet.Install (spines);

  // ── 链路 ──
  std::ostringstream qs; qs << queuePkts << "p";
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute  ("DataRate", StringValue (linkRate));
  p2p.SetChannelAttribute ("Delay",    StringValue (linkDelay));
  p2p.SetQueue ("ns3::DropTailQueue<Packet>",
                "MaxSize", QueueSizeValue (QueueSize (qs.str ())));
  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::FifoQueueDisc", "MaxSize", StringValue (qs.str ()));

  Ipv4AddressHelper ip;
  std::map<uint32_t, Ipv4Address> epIp;   // endpoint i → 第一个 p2p 接口 IP
  uint32_t subnet = 0;

  // mkLink: 建立 p2p 链路 + MacTx trace + IP 分配，返回两端 IP
  auto mkLink = [&] (Ptr<Node> u, Ptr<Node> v) -> std::pair<Ipv4Address,Ipv4Address>
  {
    NetDeviceContainer dev = p2p.Install (u, v);
    tch.Install (dev);
    dev.Get(0)->TraceConnectWithoutContext("MacTx",
      MakeCallback(+[](Ptr<const Packet> p){ g_total_bits += p->GetSize()*8; }));
    dev.Get(1)->TraceConnectWithoutContext("MacTx",
      MakeCallback(+[](Ptr<const Packet> p){ g_total_bits += p->GetSize()*8; }));
    uint32_t base = subnet * 4;
    std::ostringstream b;
    b << "10." << ((base>>16)&0xff) << "." << ((base>>8)&0xff) << "." << (base&0xff);
    ip.SetBase (b.str().c_str(), "255.255.255.252");
    Ipv4InterfaceContainer ic = ip.Assign (dev);
    ++subnet;
    return { ic.GetAddress(0), ic.GetAddress(1) };
  };

  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();

  if (isRng)
    {
      // ── RNG 拓扑：随机正则图（修复版）──
      auto edges = GenerateRRG (N_RNG, D_RNG, rng);
      for (auto &e : edges)
        {
          auto [ipU, ipV] = mkLink (endpoints.Get(e.first), endpoints.Get(e.second));
          if (!epIp.count(e.first))  epIp[e.first]  = ipU;
          if (!epIp.count(e.second)) epIp[e.second] = ipV;
        }
      std::cout << "  RNG topology: N=" << N_RNG << " D=" << D_RNG
                << " edges=" << edges.size() << "\n";
    }
  else
    {
      // ── Fat-Tree 拓扑：Leaf-Spine 全互联 ──
      // 每台 Leaf 连接所有 N_SPINE 台 Spine（全非阻塞）
      for (uint32_t l = 0; l < N_LEAF; ++l)
        for (uint32_t sp = 0; sp < N_SPINE; ++sp)
          {
            auto [ipL, ipSp] = mkLink (endpoints.Get(l), spines.Get(sp));
            if (!epIp.count(l)) epIp[l] = ipL; // 只存第一个接口 IP
          }
      std::cout << "  Fat-Tree: " << N_LEAF << " Leaf + " << N_SPINE
                << " Spine, " << (N_LEAF * N_SPINE) << " links\n";
    }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // ── 地址映射 ──
  std::map<uint32_t, Address> addr;
  for (uint32_t i = 0; i < N_ep; ++i)
    addr[i] = InetSocketAddress (epIp[i], DATA_PORT);

  // ── 安装应用 ──
  std::vector<Ptr<NetHost>> apps (N_ep);
  for (uint32_t i = 0; i < N_ep; ++i)
    {
      Ptr<Socket> rx = Socket::CreateSocket (endpoints.Get(i),
                         TypeId::LookupByName("ns3::UdpSocketFactory"));
      rx->Bind (InetSocketAddress (Ipv4Address::GetAny(), DATA_PORT));
      Ptr<NetHost> a = CreateObject<NetHost>();
      a->Setup (i, rx, addr);
      endpoints.Get(i)->AddApplication (a);
      a->SetStartTime (Seconds(0.0));
      a->SetStopTime  (Seconds(30.0));
      apps[i] = a;
    }

  // ── 调度流量（论文三种模式）──
  uint32_t nActive = std::max (2u, (uint32_t)std::ceil (f * N_ep));
  // 随机选 nActive 个活跃节点（无放回）
  std::vector<uint32_t> perm (N_ep);
  for (uint32_t i = 0; i < N_ep; ++i) perm[i] = i;
  for (uint32_t i = N_ep-1; i > 0; --i)
    { uint32_t j = rng->GetInteger(0,i); std::swap(perm[i],perm[j]); }
  std::vector<uint32_t> active (perm.begin(), perm.begin() + nActive);

  uint64_t pid = 1;
  uint64_t nFlows = 0;
  const double T0 = 1.0; // 所有流同时发出

  if (pattern == "clique")
    {
      // 论文：clique(f) — nActive 节点两两全互联
      for (uint32_t s : active)
        for (uint32_t d : active)
          if (s != d)
            {
              for (uint32_t m = 0; m < BURST_PKTS; ++m)
                Simulator::Schedule (Seconds(T0),
                  &NetHost::Send, apps[s], d, PKT_BYTES, pid++);
              ++nFlows;
            }
    }
  else if (pattern == "hubs")
    {
      // 论文：hubs(f) — nActive 个 Hub，其余所有节点向所有 Hub 发
      for (uint32_t s = 0; s < N_ep; ++s)
        for (uint32_t h : active)
          if ((uint32_t)s != h)
            {
              for (uint32_t m = 0; m < BURST_PKTS; ++m)
                Simulator::Schedule (Seconds(T0),
                  &NetHost::Send, apps[s], h, PKT_BYTES, pid++);
              ++nFlows;
            }
    }
  else // matching
    {
      // 论文：matching(f) — nActive/2 对随机配对，双向发
      uint32_t nPairs = nActive / 2;
      for (uint32_t k = 0; k < nPairs; ++k)
        {
          uint32_t s = active[k*2], d = active[k*2+1];
          for (uint32_t m = 0; m < BURST_PKTS; ++m)
            {
              Simulator::Schedule (Seconds(T0),
                &NetHost::Send, apps[s], d, PKT_BYTES, pid++);
              Simulator::Schedule (Seconds(T0),
                &NetHost::Send, apps[d], s, PKT_BYTES, pid++);
            }
          nFlows += 2;
        }
    }

  std::cout << "  pattern=" << pattern << " f=" << f
            << " nActive=" << nActive << " nFlows=" << nFlows
            << " nPkts=" << (pid-1) << "\n";

  // ── 运行 ──
  Simulator::Stop (Seconds(3.0));
  Simulator::Run ();
  Simulator::Destroy ();

  // ── 计算统计 ──
  SimResult r;
  r.sent      = g_sent;
  r.delivered = g_done.size();
  r.dropped   = (r.sent >= r.delivered) ? (r.sent - r.delivered) : 0;
  r.deliveryRate   = r.sent > 0 ? (double)r.delivered / r.sent : 0.0;
  r.oversubApprox  = r.deliveryRate > 0.001 ? 1.0 / r.deliveryRate : 99.0;

  double rxDur = (g_last_recv > g_first_recv && g_first_recv > 0)
                 ? (g_last_recv - g_first_recv) / 1e9 : 0.0;
  r.throughputGbps = rxDur > 0 ? (r.delivered * PKT_BYTES * 8.0) / (rxDur * 1e9) : 0.0;

  std::vector<double> lats;
  for (auto &p : g_done) if (p.recvNs > 0) lats.push_back (p.recvNs - p.sentNs);
  std::sort (lats.begin(), lats.end());
  r.meanUs = r.p99Us = 0.0;
  if (!lats.empty())
    {
      double sum=0; for (double v:lats) sum+=v;
      r.meanUs = sum/lats.size()/1000.0;
      r.p99Us  = lats[lats.size()*99/100]/1000.0;
    }
  return r;
}

// ─── main ──────────────────────────────────────────────────────────────────
int main (int argc, char *argv[])
{
  std::string topology  = "rng";     // "rng" | "fattree"
  std::string pattern   = "clique";  // "clique" | "hubs" | "matching"
  double      f         = 0.3;       // 活跃比例
  uint32_t    queuePkts = 8;         // 小队列 → 快速拥塞 → 清晰的丢包信号
  std::string linkRate  = "100Gbps";
  std::string linkDelay = "200ns";
  bool        scanAll   = false;     // 扫描全部 (pattern, f) 组合

  CommandLine cmd;
  cmd.AddValue ("topology",  "rng|fattree",              topology);
  cmd.AddValue ("pattern",   "clique|hubs|matching",     pattern);
  cmd.AddValue ("f",         "active fraction [0,1]",    f);
  cmd.AddValue ("queuePkts", "queue depth (packets)",    queuePkts);
  cmd.AddValue ("linkRate",  "link rate",                linkRate);
  cmd.AddValue ("linkDelay", "link delay",               linkDelay);
  cmd.AddValue ("scanAll",   "1=scan all f values → Figure 13 CSV", scanAll);
  cmd.Parse (argc, argv);

  std::cout << "=== RNG Paper Figure 13 Reproduction ===\n"
            << "  Topology : " << topology << " (N_ep="
            << (topology=="rng" ? N_RNG : N_LEAF) << ")\n"
            << "  Link     : " << linkRate << " / " << linkDelay << "\n"
            << "  Queue    : " << queuePkts
            << "p (small → quick saturation → drop rate ≈ oversubscription)\n\n";

  // 决定要扫哪些 (pattern, f) 组合
  std::vector<std::string> patterns;
  std::vector<double>      fracs;

  if (scanAll)
    {
      patterns = {"clique", "hubs", "matching"};
      // 论文 Figure 13 的 x 轴采样点
      fracs    = {0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0};
    }
  else
    {
      patterns = {pattern};
      fracs    = {f};
    }

  // 输出 CSV
  std::string csvName = "rng_fig13_" + topology + ".csv";
  std::ofstream csv (csvName);
  csv << "topology,pattern,f,sent,delivered,drop_pct,"
         "delivery_rate,oversub_approx,throughput_Gbps,mean_us,p99_us\n";

  for (const std::string &pat : patterns)
    for (double fv : fracs)
      {
        std::cout << "─── " << topology << "  pattern=" << pat
                  << "  f=" << fv << " ───\n";

        SimResult r = RunSim (topology, pat, fv, queuePkts, linkRate, linkDelay);

        printf ("  sent=%-8lu  delivered=%-8lu  drop=%.1f%%\n",
                r.sent, r.delivered,
                r.sent ? 100.0*r.dropped/r.sent : 0.0);
        printf ("  delivery_rate=%.3f  oversub≈%.2f  tput=%.2f Gbps\n",
                r.deliveryRate, r.oversubApprox, r.throughputGbps);
        printf ("  latency mean=%.1fµs  p99=%.1fµs\n\n",
                r.meanUs, r.p99Us);

        csv << topology << "," << pat << "," << fv << ","
            << r.sent << "," << r.delivered << ","
            << (r.sent ? 100.0*r.dropped/r.sent : 0.0) << ","
            << r.deliveryRate << "," << r.oversubApprox << ","
            << r.throughputGbps << "," << r.meanUs << "," << r.p99Us << "\n";
        csv.flush ();
      }

  std::cout << "wrote " << csvName << "\n\n";
  std::cout << "─── How to reproduce Figure 13 ───────────────────────────\n"
            << "  Run:\n"
            << "    ./rng-sim --topology=rng     --scanAll=1   → rng_fig13_rng.csv\n"
            << "    ./rng-sim --topology=fattree --scanAll=1   → rng_fig13_fattree.csv\n"
            << "  Plot: oversub_approx (y) vs f (x), grouped by pattern\n"
            << "  Expected: RNG ≤ Fat-Tree for clique/hubs at f>0.1\n"
            << "             RNG > Fat-Tree for matching at small f\n"
            << "────────────────────────────────────────────────────────────\n";
  return 0;
}