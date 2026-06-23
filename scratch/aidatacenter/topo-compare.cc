/*
 * topo-compare.cc  —  ns3 scratch 文件
 *
 * 用法：将此文件放入 ns3 的 scratch/ 目录，然后执行
 * ./ns3 run "topo-compare [选项]"
 * 或使用配套的 run_compare.sh 脚本
 */

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("TopoCompare");

// ═══════════════════════════════════════════════════════════════════════
//  全局统计（每次 RunSim 前清零）
// ═══════════════════════════════════════════════════════════════════════
// 将名字改为 CustomProbe，避免与 ns3::Probe 发生命名冲突
struct CustomProbe { double sentNs, recvNs; };
static std::map<uint64_t, CustomProbe> g_inflight;
static std::vector<CustomProbe>        g_done;
static uint64_t                        g_sent = 0;
static double                          g_t0   = -1, g_t1 = -1;
static uint64_t                        g_bits = 0;

static void ResetStats()
{ g_inflight.clear(); g_done.clear(); g_sent=0; g_t0=-1; g_t1=-1; g_bits=0; }

// ═══════════════════════════════════════════════════════════════════════
//  通用 Host 应用（IP 路由，不需要应用层转发）
// ═══════════════════════════════════════════════════════════════════════
class Host : public Application
{
public:
    static TypeId GetTypeId(){
        static TypeId t = TypeId("Host").SetParent<Application>()
                          .AddConstructor<Host>(); return t;
    }
    void Setup(uint32_t id, Ptr<Socket> rx, std::map<uint32_t,Address> addr)
    { m_id=id; m_rx=rx; m_addr=std::move(addr); }

    void Send(uint32_t dst, uint32_t bytes, uint64_t id){
        if(m_id==dst) return;
        Ptr<Packet> pkt = Create<Packet>(bytes);
        uint8_t b[12]; memcpy(b,&id,8); memcpy(b+8,&dst,4);
        pkt->AddAtEnd(Create<Packet>(b,12));
        CustomProbe pr; pr.sentNs=Simulator::Now().GetNanoSeconds(); pr.recvNs=0;
        g_inflight[id]=pr; g_sent++;
        auto it=m_addr.find(dst); if(it==m_addr.end()) return;
        if(!m_tx) m_tx=Socket::CreateSocket(GetNode(),
                       TypeId::LookupByName("ns3::UdpSocketFactory"));
        m_tx->SendTo(pkt,0,it->second);
    }
private:
    void StartApplication() override
    { m_rx->SetRecvCallback(MakeCallback(&Host::OnRecv,this)); }
    void StopApplication() override {}
    void OnRecv(Ptr<Socket> s){
        Ptr<Packet> pkt;
        while((pkt=s->Recv())){
            if(pkt->GetSize()<12) continue;
            uint8_t b[12];
            pkt->CreateFragment(pkt->GetSize()-12,12)->CopyData(b,12);
            uint64_t id; memcpy(&id,b,8);
            double now=Simulator::Now().GetNanoSeconds();
            if(g_t0<0) { g_t0=now; } 
            g_t1=now;
            auto it=g_inflight.find(id);
            if(it!=g_inflight.end())
            { it->second.recvNs=now; g_done.push_back(it->second); g_inflight.erase(it); }
        }
    }
    uint32_t m_id{0}; Ptr<Socket> m_rx,m_tx; std::map<uint32_t,Address> m_addr;
};

// ═══════════════════════════════════════════════════════════════════════
//  生成随机正则图（configuration model）
// ═══════════════════════════════════════════════════════════════════════
static std::vector<std::pair<uint32_t,uint32_t>>
MakeRRG(uint32_t n, uint32_t d, Ptr<UniformRandomVariable> rng)
{
    std::vector<std::pair<uint32_t,uint32_t>> edges;
    for(int attempt=1;;attempt++){
        std::vector<uint32_t> stubs; stubs.reserve(n*d);
        for(uint32_t i=0;i<n;i++) for(uint32_t j=0;j<d;j++) stubs.push_back(i);
        for(int64_t i=(int64_t)stubs.size()-1;i>0;i--)
        { uint32_t j=rng->GetInteger(0,(uint32_t)i); std::swap(stubs[i],stubs[j]); }
        bool ok=true;
        std::set<std::pair<uint32_t,uint32_t>> seen; edges.clear();
        for(size_t i=0;i+1<stubs.size();i+=2){
            uint32_t u=stubs[i],v=stubs[i+1];
            if(u==v){ok=false;break;}
            auto k=std::make_pair(std::min(u,v),std::max(u,v));
            if(seen.count(k)){ok=false;break;}
            seen.insert(k); edges.push_back({u,v});
        }
        if(ok){ std::cout<<"  [RRG] built in "<<attempt<<" attempt(s)\n"; break; }
    }
    return edges;
}

// ═══════════════════════════════════════════════════════════════════════
//  仿真结果
// ═══════════════════════════════════════════════════════════════════════
struct Result {
    std::string topo, pattern; double f;
    uint64_t sent,delivered,dropped;
    double dropPct,delivRate,oversub,tputGbps,meanUs,p99Us,energyJ,avgPowerW;
};

// ═══════════════════════════════════════════════════════════════════════
//  核心：建拓扑 + 安装流量 + 运行 + 统计
// ═══════════════════════════════════════════════════════════════
static Result RunSim(
    const std::string &topo,
    const std::string &pattern,
    double             f,
    uint32_t           queuePkts,
    const std::string &linkRate,
    const std::string &linkDelay,
    uint32_t           pktBytes,
    uint32_t           burstPkts)
{
    ResetStats();
    Config::SetDefault("ns3::Ipv4GlobalRouting::RandomEcmpRouting",BooleanValue(true));

    uint32_t N_ep = 0;       
    uint32_t N_sw = 0;       
    double   P_sw = 300.0;   
    Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();

    NodeContainer eps;   
    NodeContainer sw1, sw2; 

    std::ostringstream qs; qs<<queuePkts<<"p";
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate",StringValue(linkRate));
    p2p.SetChannelAttribute("Delay",StringValue(linkDelay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>",
                 "MaxSize",QueueSizeValue(QueueSize(qs.str())));
    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::FifoQueueDisc","MaxSize",StringValue(qs.str()));

    Ipv4AddressHelper ipHelper;
    uint32_t subnet=0;
    std::map<uint32_t,Ipv4Address> epIp;
    InternetStackHelper inet;

    auto mkLink=[&](Ptr<Node> u,Ptr<Node> v)->std::pair<Ipv4Address,Ipv4Address>{
        NetDeviceContainer dev=p2p.Install(u,v); tch.Install(dev);
        dev.Get(0)->TraceConnectWithoutContext("MacTx",
            MakeCallback(+[](Ptr<const Packet> p){g_bits+=p->GetSize()*8;}));
        dev.Get(1)->TraceConnectWithoutContext("MacTx",
            MakeCallback(+[](Ptr<const Packet> p){g_bits+=p->GetSize()*8;}));
        uint32_t base=subnet*4;
        std::ostringstream b;
        b<<"10."<<((base>>16)&0xff)<<"."<<((base>>8)&0xff)<<"."<<(base&0xff);
        ipHelper.SetBase(b.str().c_str(),"255.255.255.252");
        Ipv4InterfaceContainer ic=ipHelper.Assign(dev);
        ++subnet;
        return {ic.GetAddress(0),ic.GetAddress(1)};
    };

    if(topo=="rng"){
        N_ep=100;
        eps.Create(N_ep); inet.Install(eps);
        auto edges=MakeRRG(N_ep,16,rng);
        for(auto &e:edges){
            auto [iA,iB]=mkLink(eps.Get(e.first),eps.Get(e.second));
            if(!epIp.count(e.first))  epIp[e.first]=iA;
            if(!epIp.count(e.second)) epIp[e.second]=iB;
        }
    }
    else if(topo=="fattree"){
        const uint32_t K=14, Npod=K, Ntor=K/2, Nagg=K/2, Ncore=(K/2)*(K/2);
        uint32_t N_tor_total = Npod*Ntor;  
        N_ep = N_tor_total;
        N_sw = Npod*Nagg + Ncore;
        eps.Create(N_ep);  
        sw1.Create(Npod*Nagg);  
        sw2.Create(Ncore);      
        inet.Install(eps); inet.Install(sw1); inet.Install(sw2);
        for(uint32_t pod=0;pod<Npod;pod++)
          for(uint32_t t=0;t<Ntor;t++)
            for(uint32_t a=0;a<Nagg;a++){
                auto [iT,iA]=mkLink(eps.Get(pod*Ntor+t), sw1.Get(pod*Nagg+a));
                if(!epIp.count(pod*Ntor+t)) epIp[pod*Ntor+t]=iT;
            }
        for(uint32_t pod=0;pod<Npod;pod++)
          for(uint32_t a=0;a<Nagg;a++)
            for(uint32_t c=0;c<K/2;c++)
              mkLink(sw1.Get(pod*Nagg+a), sw2.Get(a*(K/2)+c));
    }
    else if(topo=="leafspine"){
        const uint32_t nLeaf=10, nSpine=10, nSrv=10; 
        N_ep = nLeaf*nSrv;
        N_sw = nLeaf+nSpine;
        NodeContainer srvs,leaves,spines;
        srvs.Create(N_ep); leaves.Create(nLeaf); spines.Create(nSpine);
        inet.Install(srvs); inet.Install(leaves); inet.Install(spines);
        for(uint32_t l=0;l<nLeaf;l++)
          for(uint32_t s=0;s<nSrv;s++){
              uint32_t sid=l*nSrv+s;
              auto [iS,iL]=mkLink(srvs.Get(sid),leaves.Get(l));
              if(!epIp.count(sid)) epIp[sid]=iS;
          }
        for(uint32_t l=0;l<nLeaf;l++)
          for(uint32_t sp=0;sp<nSpine;sp++)
            mkLink(leaves.Get(l),spines.Get(sp));
        eps=srvs;
    }
    else{ 
        const uint32_t A=3,P=3,H=3,G=A*H+1;  
        N_ep=P*A*G;  
        uint32_t nRouters=A*G; 
        NodeContainer srvs,routers;
        srvs.Create(N_ep); routers.Create(nRouters);
        inet.Install(srvs); inet.Install(routers);
        N_sw=nRouters;

        auto rIdx=[&](uint32_t g,uint32_t r){return g*A+r;};
        auto sIdx=[&](uint32_t g,uint32_t r,uint32_t s){return (g*A+r)*P+s;};

        for(uint32_t g=0;g<G;g++) for(uint32_t r=0;r<A;r++) for(uint32_t s=0;s<P;s++){
            uint32_t sid=sIdx(g,r,s);
            auto [iS,iR]=mkLink(srvs.Get(sid),routers.Get(rIdx(g,r)));
            if(!epIp.count(sid)) epIp[sid]=iS;
        }
        for(uint32_t g=0;g<G;g++) for(uint32_t r1=0;r1<A;r1++) for(uint32_t r2=r1+1;r2<A;r2++)
            mkLink(routers.Get(rIdx(g,r1)),routers.Get(rIdx(g,r2)));
        for(uint32_t gi=0;gi<G;gi++) for(uint32_t gj=gi+1;gj<G;gj++){
            uint32_t ri=(gj-gi-1)%A;
            uint32_t rj=(G-(gj-gi)-1)%A;
            mkLink(routers.Get(rIdx(gi,ri)),routers.Get(rIdx(gj,rj)));
        }
        eps=srvs;
    }

    std::cout<<"  ["<<topo<<"] N_ep="<<N_ep<<" links="<<subnet<<"\n";

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    std::map<uint32_t,Address> addr;
    for(uint32_t i=0;i<N_ep;i++)
        addr[i]=InetSocketAddress(epIp[i],9000);

    std::vector<Ptr<Host>> apps(N_ep);
    for(uint32_t i=0;i<N_ep;i++){
        Ptr<Socket> rx=Socket::CreateSocket(eps.Get(i),
                         TypeId::LookupByName("ns3::UdpSocketFactory"));
        rx->Bind(InetSocketAddress(Ipv4Address::GetAny(),9000));
        Ptr<Host> a=CreateObject<Host>(); a->Setup(i,rx,addr);
        eps.Get(i)->AddApplication(a);
        a->SetStartTime(Seconds(0)); a->SetStopTime(Seconds(30));
        apps[i]=a;
    }

    uint32_t nActive=std::max(2u,(uint32_t)std::ceil(f*N_ep));
    std::vector<uint32_t> perm(N_ep); for(uint32_t i=0;i<N_ep;i++) perm[i]=i;
    for(uint32_t i=N_ep-1;i>0;i--)
    { uint32_t j=rng->GetInteger(0,i); std::swap(perm[i],perm[j]); }
    std::vector<uint32_t> active(perm.begin(),perm.begin()+nActive);

    uint64_t pid=1;
    const double T0=1.0;

    if(pattern=="clique"){
        for(uint32_t s:active) for(uint32_t d:active) if(s!=d)
            for(uint32_t m=0;m<burstPkts;m++)
                Simulator::Schedule(Seconds(T0),&Host::Send,apps[s],d,pktBytes,pid++);
    } else if(pattern=="hubs"){
        for(uint32_t s=0;s<N_ep;s++) for(uint32_t h:active) if(s!=h)
            for(uint32_t m=0;m<burstPkts;m++)
                Simulator::Schedule(Seconds(T0),&Host::Send,apps[s],h,pktBytes,pid++);
    } else if(pattern=="matching"){
        uint32_t nPairs=nActive/2;
        for(uint32_t k=0;k<nPairs;k++){
            uint32_t s=active[k*2],d=active[k*2+1];
            for(uint32_t m=0;m<burstPkts;m++){
                Simulator::Schedule(Seconds(T0),&Host::Send,apps[s],d,pktBytes,pid++);
                Simulator::Schedule(Seconds(T0),&Host::Send,apps[d],s,pktBytes,pid++);
            }
        }
    } else{ 
        double when=T0;
        for(uint32_t k=0;k<(uint32_t)(burstPkts*nActive);k++){
            uint32_t s=rng->GetInteger(0,N_ep-1),d=rng->GetInteger(0,N_ep-1);
            if(s==d){k--;continue;}
            Simulator::Schedule(Seconds(when),&Host::Send,apps[s],d,pktBytes,pid++);
            when+=5e-7;
        }
    }

    Simulator::Stop(Seconds(3.0));
    Simulator::Run();
    Simulator::Destroy();

    Result r;
    r.topo=topo; r.pattern=pattern; r.f=f;
    r.sent      = g_sent;
    r.delivered = g_done.size();
    r.dropped   = r.sent>=r.delivered ? r.sent-r.delivered : 0;
    r.dropPct   = r.sent>0 ? 100.0*r.dropped/r.sent : 0.0;
    r.delivRate = r.sent>0 ? (double)r.delivered/r.sent : 0.0;
    r.oversub   = r.delivRate>0.001 ? 1.0/r.delivRate : 99.0;

    double dur=(g_t1>g_t0&&g_t0>0)?(g_t1-g_t0)/1e9:0.0;
    r.tputGbps  = dur>0 ? (r.delivered*pktBytes*8.0)/(dur*1e9) : 0.0;

    std::vector<double> lats;
    for(size_t i = 0; i < g_done.size(); ++i) {
        if(g_done[i].recvNs > 0) lats.push_back(g_done[i].recvNs - g_done[i].sentNs);
    }
    std::sort(lats.begin(),lats.end());
    r.meanUs=r.p99Us=0.0;
    if(!lats.empty()){
        double sum=0; for(double v:lats) sum+=v;
        r.meanUs=sum/lats.size()/1000.0;
        r.p99Us =lats[lats.size()*99/100]/1000.0;
    }

    double simDur=1.5;
    double Psrv_static=2000.0, Psrv_full=8000.0, Psw=P_sw, Ebit=10e-12;
    double E_static=(N_ep*Psrv_static + N_sw*Psw)*simDur;
    double E_comp  = dur>0 ? N_ep*(Psrv_full-Psrv_static)*dur : 0.0;
    double E_net   = g_bits*Ebit;
    r.energyJ   = E_static+E_comp+E_net;
    r.avgPowerW = r.energyJ/simDur;

    return r;
}

static void PrintHeader(){
    printf("\n%-12s %-10s %4s | %7s %7s %6s | %7s %6s %6s | %10s %8s\n",
           "Topology","Pattern","f",
           "Sent","Deliv","Drop%",
           "Tput(G)","p50µs","p99µs",
           "Energy(J)","Power(W)");
    printf("%s\n",std::string(110,'-').c_str());
}

static void PrintRow(const Result &r){
    printf("%-12s %-10s %4.2f | %7lu %7lu %5.1f%% | %7.2f %6.1f %6.1f | %10.0f %8.0f\n",
           r.topo.c_str(), r.pattern.c_str(), r.f,
           r.sent, r.delivered, r.dropPct,
           r.tputGbps, r.meanUs, r.p99Us,
           r.energyJ, r.avgPowerW);
}

static void PrintGroupSummary(const std::vector<Result> &results,
                               const std::string &pat, double f)
{
    printf("\n  [%s f=%.2f]  过订阅比（越低越好）：",pat.c_str(),f);
    for(size_t i = 0; i < results.size(); ++i) {
        if(results[i].pattern==pat && std::abs(results[i].f-f)<1e-6)
            printf("  %s=%.2f",results[i].topo.c_str(),results[i].oversub);
    }
    printf("\n");
}

int main(int argc, char *argv[])
{
    std::string topoArg    = "all";
    std::string patternArg = "all";
    double      fArg       = 0.3;
    uint32_t    queuePkts  = 8;
    uint32_t    pktBytes   = 1024;
    uint32_t    burstPkts  = 64;
    std::string linkRate   = "100Gbps";
    std::string linkDelay  = "200ns";
    bool        scanF      = false;

    CommandLine cmd;
    cmd.AddValue("topo",      "rng|fattree|leafspine|dragonfly|all", topoArg);
    cmd.AddValue("pattern",   "clique|hubs|matching|uniform|all",    patternArg);
    cmd.AddValue("f",         "active fraction 0~1",                 fArg);
    cmd.AddValue("scanF",     "1=scan multiple f values",            scanF);
    cmd.AddValue("queuePkts", "queue depth (packets)",               queuePkts);
    cmd.AddValue("pktBytes",  "packet size (bytes)",                 pktBytes);
    cmd.AddValue("burstPkts", "burst packets per flow",              burstPkts);
    cmd.AddValue("linkRate",  "link rate",                           linkRate);
    cmd.AddValue("linkDelay", "link delay",                          linkDelay);
    cmd.Parse(argc, argv);

    std::vector<std::string> topos;
    if(topoArg=="all") topos={"rng","fattree","leafspine","dragonfly"};
    else               topos={topoArg};

    std::vector<std::string> patterns;
    if(patternArg=="all") patterns={"clique","hubs","matching"};
    else                  patterns={patternArg};

    std::vector<double> fVals;
    if(scanF) fVals={0.1,0.2,0.3,0.5,0.8,1.0};
    else      fVals={fArg};

    std::cout<<"================================================================\n"
             <<"   Datacenter Topology Comparison (RNG Paper Figure 13 style)   \n"
             <<"================================================================\n"
             <<"  Topologies : "<<topoArg<<"\n"
             <<"  Patterns   : "<<patternArg<<"\n"
             <<"  f values   : ";
    for(size_t i = 0; i < fVals.size(); ++i) std::cout<<fVals[i]<<" "; 
    std::cout<<"\n"<<"  Link       : "<<linkRate<<" / "<<linkDelay<<"\n"
             <<"  Queue      : "<<queuePkts<<"p\n\n";

    std::vector<Result> allResults;
    std::ofstream csv("topo_compare_results.csv");
    csv<<"topology,pattern,f,sent,delivered,drop_pct,delivery_rate,"
         "oversub_approx,throughput_Gbps,mean_us,p99_us,energy_J,avg_power_W\n";

    PrintHeader();

    for(size_t p_idx = 0; p_idx < patterns.size(); ++p_idx) {
      std::string pat = patterns[p_idx];
      for(size_t f_idx = 0; f_idx < fVals.size(); ++f_idx) {
        double fv = fVals[f_idx];
        std::cout<<"\n══ pattern="<<pat<<"  f="<<fv<<" ══\n";
        for(size_t t_idx = 0; t_idx < topos.size(); ++t_idx) {
            std::string tp = topos[t_idx];
            std::cout<<"  Building "<<tp<<"...\n";
            Result r = RunSim(tp, pat, fv, queuePkts, linkRate, linkDelay,
                              pktBytes, burstPkts);
            allResults.push_back(r);
            PrintRow(r);

            csv<<r.topo<<","<<r.pattern<<","<<r.f<<","
               <<r.sent<<","<<r.delivered<<","<<r.dropPct<<","
               <<r.delivRate<<","<<r.oversub<<","<<r.tputGbps<<","
               <<r.meanUs<<","<<r.p99Us<<","<<r.energyJ<<","<<r.avgPowerW<<"\n";
            csv.flush();
          }
        PrintGroupSummary(allResults, pat, fv);
      }
    }

    csv.close();

    std::cout<<"\n========================================================================================\n";
    std::cout<<"  SUMMARY: 过订阅比 oversub ≈ 1/delivery_rate（越低代表网络越高效）\n";
    std::cout<<"----------------------------------------------------------------------------------------\n";
    printf("  %-12s %-10s %4s  →  %s\n","Topology","Pattern","f","oversub (lower=better)");
    std::cout<<"----------------------------------------------------------------------------------------\n";
    for(size_t i = 0; i < allResults.size(); ++i) {
        printf("  %-12s %-10s %4.2f  →  %.3f  (delivery %.1f%%  tput %.2f Gbps)\n",
               allResults[i].topo.c_str(), allResults[i].pattern.c_str(), allResults[i].f,
               allResults[i].oversub, allResults[i].delivRate*100, allResults[i].tputGbps);
    }

    std::cout<<"\n  Wrote: topo_compare_results.csv\n";
    return 0;
}