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

#include "mesh-route-header.h"

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("Mesh4dSim");

// W=6 代表四维网格每条边有6个节点，全网共 6^4 = 1296 台服务器
static const uint32_t W = 6;

static uint32_t NID (uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
  return (((x * W + y) * W + z) * W) + w;
}
static void XYZW (uint32_t i, uint32_t &x, uint32_t &y, uint32_t &z, uint32_t &w)
{
  w = i % W; i /= W;
  z = i % W; i /= W;
  y = i % W;
  x = i / W;
}

// 四维维度渐进路由算法 (4D DOR: X -> Y -> Z -> W)
static std::vector<uint16_t> Dor4 (uint32_t s, uint32_t d)
{
  uint32_t sx, sy, sz, sw, dx, dy, dz, dw;
  XYZW (s, sx, sy, sz, sw);
  XYZW (d, dx, dy, dz, dw);
  std::vector<uint16_t> path;
  path.push_back ((uint16_t) s);

  uint32_t cx = sx, cy = sy, cz = sz, cw = sw;
  if (cx != dx) { cx = dx; path.push_back ((uint16_t) NID (cx, cy, cz, cw)); }
  if (cy != dy) { cy = dy; path.push_back ((uint16_t) NID (cx, cy, cz, cw)); }
  if (cz != dz) { cz = dz; path.push_back ((uint16_t) NID (cx, cy, cz, cw)); }
  if (cw != dw) { cw = dw; path.push_back ((uint16_t) NID (cx, cy, cz, cw)); }
  return path;
}

struct MeshProbe
{
  double sentNs;
  double recvNs;
  uint16_t relays;
};
static std::map<uint64_t, MeshProbe> g_inflight;
static std::vector<MeshProbe> g_done;
static uint64_t g_sent = 0;

static double g_first_recv_time = -1.0;
static double g_last_recv_time = -1.0;
// FIX: 改由 MacTx trace 写入，与 fat-tree/RRG 统计口径一致
static uint64_t g_total_bits_transmitted = 0;

class MeshHost : public Application
{
public:
  static TypeId GetTypeId ()
  {
    static TypeId t = TypeId ("MeshHost")
      .SetParent<Application> ()
      .AddConstructor<MeshHost> ();
    return t;
  }

  void Setup (uint32_t id, Ptr<Socket> rx, std::map<uint32_t, Address> addr)
  {
    m_id = id;
    m_rx = rx;
    m_addr = std::move (addr);
  }

  // FIX: 加 src==dst guard，防止 path[1] 越界
  void Send (uint32_t dst, uint32_t bytes, uint64_t id)
  {
    if (m_id == dst) return;
    std::vector<uint16_t> path = Dor4 (m_id, dst);
    MeshRouteHeader h;
    h.SetPath (path);
    Ptr<Packet> pkt = Create<Packet> (bytes);
    pkt->AddHeader (h);
    AppendId (pkt, id, dst);
    uint16_t relays = (path.size () >= 2) ? (uint16_t) (path.size () - 2) : 0;
    MeshProbe pr;
    pr.sentNs = Simulator::Now ().GetNanoSeconds ();
    pr.recvNs = 0;
    pr.relays = relays;
    g_inflight[id] = pr;
    g_sent++;
    SendTo (path[1], pkt);
  }

private:
  void StartApplication () override
  {
    m_rx->SetRecvCallback (MakeCallback (&MeshHost::OnRecv, this));
  }
  void StopApplication () override {}

  static void AppendId (Ptr<Packet> p, uint64_t id, uint32_t dst)
  {
    uint8_t b[12];
    std::memcpy (b, &id, 8);
    std::memcpy (b + 8, &dst, 4);
    p->AddAtEnd (Create<Packet> (b, 12));
  }

  void OnRecv (Ptr<Socket> s)
  {
    Ptr<Packet> pkt;
    while ((pkt = s->Recv ()))
      {
        uint64_t id = 0;
        uint32_t fdst = 0;
        if (pkt->GetSize () >= 12)
          {
            uint8_t b[12];
            Ptr<Packet> t = pkt->CreateFragment (pkt->GetSize () - 12, 12);
            t->CopyData (b, 12);
            std::memcpy (&id, b, 8);
            std::memcpy (&fdst, b + 8, 4);
            pkt->RemoveAtEnd (12);
          }
        MeshRouteHeader h;
        pkt->RemoveHeader (h);
        const std::vector<uint16_t> &path = h.GetPath ();

        if (m_id == path.back())
          {
            double nowNs = Simulator::Now ().GetNanoSeconds ();
            if (g_first_recv_time < 0) g_first_recv_time = nowNs;
            g_last_recv_time = nowNs;

            std::map<uint64_t, MeshProbe>::iterator it = g_inflight.find (id);
            if (it != g_inflight.end ())
              {
                it->second.recvNs = nowNs;
                g_done.push_back (it->second);
                g_inflight.erase (it);
              }
            continue;
          }

        uint16_t next_hop = 0;
        bool found = false;
        for (size_t i = 0; i < path.size() - 1; ++i)
          {
            if (path[i] == m_id)
              {
                next_hop = path[i + 1];
                found = true;
                h.SetCursor(i + 1);
                break;
              }
          }

        if (found)
          {
            pkt->AddHeader (h);
            AppendId (pkt, id, fdst);
            SendTo (next_hop, pkt);
          }
      }
  }

  void SendTo (uint16_t nid, Ptr<Packet> pkt)
  {
    std::map<uint32_t, Address>::iterator it = m_addr.find (nid);
    if (it == m_addr.end ()) return;
    if (!m_tx)
      m_tx = Socket::CreateSocket (GetNode (),
               TypeId::LookupByName ("ns3::UdpSocketFactory"));
    // FIX: 删除手动累加，改由 MacTx trace 统计
    m_tx->SendTo (pkt, 0, it->second);
  }

  uint32_t m_id {0};
  Ptr<Socket> m_rx;
  Ptr<Socket> m_tx;
  std::map<uint32_t, Address> m_addr;
};

int main (int argc, char *argv[])
{
  std::string scenario = "uniform";
  bool schedule = true;
  uint32_t pktBytes = 1024;
  uint32_t queuePkts = 8;
  uint32_t uniformFlows = 200000;
  std::string linkRate = "100Gbps";
  std::string linkDelay = "200ns";

  CommandLine cmd;
  cmd.AddValue ("scenario", "uniform|relaycongest", scenario);
  cmd.AddValue ("schedule", "1=paced (lossless), 0=burst (drops)", schedule);
  cmd.AddValue ("pktBytes", "payload bytes", pktBytes);
  cmd.AddValue ("queuePkts", "finite queue depth (packets)", queuePkts);
  cmd.AddValue ("uniformFlows", "number of random flows", uniformFlows);
  cmd.AddValue ("linkRate", "link rate", linkRate);
  cmd.AddValue ("linkDelay", "link delay", linkDelay);
  cmd.Parse (argc, argv);

  uint32_t N = W * W * W * W;
  std::cout << "building 4D mesh: " << W << "^4 = " << N << " hosts ...\n";

  NodeContainer hosts;
  hosts.Create (N);
  InternetStackHelper inet;
  inet.Install (hosts);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue (linkRate));
  p2p.SetChannelAttribute ("Delay", StringValue (linkDelay));
  std::ostringstream qs;
  qs << queuePkts << "p";
  p2p.SetQueue ("ns3::DropTailQueue<Packet>",
                "MaxSize", QueueSizeValue (QueueSize (qs.str ())));

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::FifoQueueDisc",
                        "MaxSize", StringValue (qs.str ()));

  Ipv4AddressHelper ip;
  std::map<uint32_t, Address> addr;
  std::map<uint32_t, Ipv4Address> firstIp;
  const uint16_t PORT = 9000;
  uint32_t subnet = 0;

  std::function<void(uint32_t, uint32_t)> addEdge =
    [&] (uint32_t u, uint32_t v)
    {
      if (v <= u) return;
      NetDeviceContainer dev = p2p.Install (hosts.Get (u), hosts.Get (v));
      tch.Install (dev);
      // FIX: MacTx trace 统计链路层比特数
      dev.Get (0)->TraceConnectWithoutContext ("MacTx",
        MakeCallback (+[](Ptr<const Packet> p){ g_total_bits_transmitted += p->GetSize () * 8; }));
      dev.Get (1)->TraceConnectWithoutContext ("MacTx",
        MakeCallback (+[](Ptr<const Packet> p){ g_total_bits_transmitted += p->GetSize () * 8; }));
      uint32_t base = subnet * 4;
      std::ostringstream b;
      b << "10." << ((base >> 16) & 0xff) << "." << ((base >> 8) & 0xff)
        << "." << (base & 0xff);
      ip.SetBase (b.str ().c_str (), "255.255.255.252");
      Ipv4InterfaceContainer ic = ip.Assign (dev);
      if (!firstIp.count (u)) firstIp[u] = ic.GetAddress (0);
      if (!firstIp.count (v)) firstIp[v] = ic.GetAddress (1);
      ++subnet;
    };

  for (uint32_t x = 0; x < W; ++x)
    for (uint32_t y = 0; y < W; ++y)
      for (uint32_t z = 0; z < W; ++z)
        for (uint32_t w = 0; w < W; ++w)
          {
            uint32_t u = NID (x, y, z, w);
            for (uint32_t x2 = 0; x2 < W; ++x2)
              if (x2 != x) addEdge (u, NID (x2, y, z, w));
            for (uint32_t y2 = 0; y2 < W; ++y2)
              if (y2 != y) addEdge (u, NID (x, y2, z, w));
            for (uint32_t z2 = 0; z2 < W; ++z2)
              if (z2 != z) addEdge (u, NID (x, y, z2, w));
            for (uint32_t w2 = 0; w2 < W; ++w2)
              if (w2 != w) addEdge (u, NID (x, y, z, w2));
          }
  std::cout << "  4D links built: " << subnet << "\n";

  std::cout << "  populating routing tables (this WILL take a moment for 1296 nodes)... \n";
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  for (uint32_t i = 0; i < N; ++i)
    addr[i] = InetSocketAddress (firstIp[i], PORT);

  std::vector<Ptr<MeshHost>> apps (N);
  for (uint32_t i = 0; i < N; ++i)
    {
      Ptr<Socket> rx = Socket::CreateSocket (hosts.Get (i),
                          TypeId::LookupByName ("ns3::UdpSocketFactory"));
      rx->Bind (InetSocketAddress (Ipv4Address::GetAny (), PORT));
      Ptr<MeshHost> a = CreateObject<MeshHost> ();
      a->Setup (i, rx, addr);
      hosts.Get (i)->AddApplication (a);
      a->SetStartTime (Seconds (0.0));
      a->SetStopTime (Seconds (30.0));
      apps[i] = a;
    }

  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable> ();
  uint64_t id = 1;
  double serSec = (pktBytes * 8.0) / 100e9;

  if (scenario == "relaycongest")
    {
      uint32_t dy = 3, dz = 3, dw = 3, DX = 4;
      uint32_t D = NID (DX, dy, dz, dw);
      double base = 1.0;
      uint32_t flow = 0;
      for (uint32_t x = 0; x < W; ++x)
        {
          if (x == DX) continue;
          uint32_t s = NID (x, dy, dz, dw);
          for (uint32_t m = 0; m < 256; ++m)
            {
              double when = schedule ? base + (flow * 256 + m) * serSec
                                     : base + (m * 1e-9);
              Simulator::Schedule (Seconds (when), &MeshHost::Send, apps[s],
                                   D, pktBytes, id++);
            }
          ++flow;
        }
    }
  else
    {
      double when = 1.0;
      for (uint32_t k = 0; k < uniformFlows; ++k)
        {
          uint32_t s = rng->GetInteger (0, N - 1);
          uint32_t d = rng->GetInteger (0, N - 1);
          if (s == d) continue;
          Simulator::Schedule (Seconds (when), &MeshHost::Send, apps[s],
                               d, pktBytes, id++);
          when += 5e-7;
        }
    }

  double simDuration = 1.5;
  Simulator::Stop (Seconds (simDuration));
  std::cout << "running simulation ...\n";
  Simulator::Run ();
  Simulator::Destroy ();

  uint64_t delivered = g_done.size ();
  uint64_t dropped = (g_sent >= delivered) ? (g_sent - delivered) : 0;

  double rxDurationSec = 0.0;
  double throughputGbps = 0.0;
  if (g_last_recv_time > g_first_recv_time && g_first_recv_time > 0)
    {
      rxDurationSec = (g_last_recv_time - g_first_recv_time) / 1e9;
      throughputGbps = (delivered * pktBytes * 8.0) / (rxDurationSec * 1e9);
    }

  double P_static_node = 2000.0;
  double P_full_load   = 8000.0;
  double P_compute     = P_full_load - P_static_node;
  double E_dynamic_bit = 10e-12;

  double staticEnergy = N * P_static_node * simDuration;
  double computeEnergy = 0.0;
  if (rxDurationSec > 0)
    computeEnergy = N * P_compute * rxDurationSec;
  double dynamicNetworkEnergy = g_total_bits_transmitted * E_dynamic_bit;
  double totalEnergy = staticEnergy + computeEnergy + dynamicNetworkEnergy;
  double avgPowerW   = totalEnergy / simDuration;

  std::map<uint16_t, std::vector<double>> byRelay;
  for (uint32_t i = 0; i < g_done.size (); ++i)
    if (g_done[i].recvNs > 0)
      byRelay[g_done[i].relays].push_back (g_done[i].recvNs - g_done[i].sentNs);

  std::cout << "\n=== 4D mesh " << W << "^4=" << N
            << " scenario=" << scenario << " schedule=" << schedule
            << " queue=" << queuePkts << "p ===\n";
  std::cout << "sent=" << g_sent << " delivered=" << delivered
            << " dropped=" << dropped
            << " (" << (g_sent ? (100.0 * dropped / g_sent) : 0) << "%)\n";

  std::cout << "-------------------------------------------\n";
  std::cout << "吞吐量 (Throughput)     : " << throughputGbps << " Gbps\n";
  std::cout << "接收有效时长 (Duration)  : " << rxDurationSec << " s\n";
  std::cout << "全网总能耗 (Total Energy): " << totalEnergy << " J\n";
  std::cout << "  ├─ 服务器基础静态能耗(Static) : " << staticEnergy << " J (2000W 基础)\n";
  std::cout << "  ├─ GPU满载额外计算能耗(Compute): " << computeEnergy << " J (突发 8000W 增量)\n";
  std::cout << "  └─ 网卡/光模块动态传输(Network): " << dynamicNetworkEnergy << " J\n";
  std::cout << "全网平均总功耗 (Avg Power): " << avgPowerW << " W\n";
  std::cout << "-------------------------------------------\n";

  std::cout << "relay  count   mean_us   p99_us   max_us\n";
  for (std::map<uint16_t, std::vector<double>>::iterator kv = byRelay.begin ();
       kv != byRelay.end (); ++kv)
    {
      std::vector<double> v = kv->second;
      std::sort (v.begin (), v.end ());
      double sum = 0;
      for (uint32_t i = 0; i < v.size (); ++i) sum += v[i];
      double mean = sum / v.size ();
      double p99 = v[std::min ((size_t) (v.size () - 1), v.size () * 99 / 100)];
      double max_val = v.back ();
      printf ("%3u   %6zu  %8.3f  %8.3f  %8.3f\n", kv->first, v.size (),
              mean / 1000, p99 / 1000, max_val / 1000);
    }

  std::ofstream csv ("mesh4d_result.csv");
  csv << "scenario,schedule,sent,delivered,dropped,throughput_Gbps,total_energy_J,avg_power_W\n";
  csv << scenario << "," << schedule << ","
      << g_sent << "," << delivered << "," << dropped << ","
      << throughputGbps << "," << totalEnergy << "," << avgPowerW << "\n";
  std::cout << "wrote mesh4d_result.csv\n";
  return 0;
}
