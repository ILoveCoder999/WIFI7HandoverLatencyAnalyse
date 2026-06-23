/*
 * test_energy.cc — 独立验证 energy-model.h（无需 ns-3）
 * 用每个拓扑的真实规模构造清单，打印功耗摘要并写统一 CSV。
 * 同时演示每个拓扑该如何"填清单"。
 *   g++ -std=c++14 -O2 test_energy.cc -o test_energy && ./test_energy
 */
#include "energy-model.h"

// 用占位的吞吐数让 pJ/bit 指标可计算（真实运行时由各 sim 传入实测值）。
// durSec、linkBits、delivered 这里用代表性量级，便于看出相对关系。
static void run (const std::string &topo, const std::string &scen,
                 EnergyInventory inv, double durSec,
                 uint64_t linkBits, uint64_t deliveredPkts, uint32_t pktBytes,
                 double tputGbps)
{
  EnergyModel em;
  em.inv = inv;
  em.SetDuration (durSec);
  em.SetBits (linkBits);
  em.SetTraffic (deliveredPkts, pktBytes);
  em.SetThroughput (tputGbps);              // 实测稳态吞吐
  em.PrintSummary (topo, scen);
  em.WriteCsv ("energy_unified.csv", topo, scen);
}

int main ()
{
  std::remove ("energy_unified.csv");
  const uint32_t PKT = 1024;
  const double   T   = 2.0;          // 代表性稳态时长(s)
  const uint64_t LB  = 5e13;         // 代表性全网链路比特
  const uint64_t DLV = 4e9;          // 代表性交付包数
  // 代表性实测吞吐(Gbps)：真实运行换成各 sim 的 tputGbps。这里设为
  // ≈0.8×对分带宽，便于演示 bisecUtil / Gbps-per-W 指标。

  // ── 1) 三层胖树 K=8: 8 pod ×(4 ToR,4 Aggr), 16 Core, N=128 ──
  //   L2(ToR-Aggr)=8*4*4=128, L3(Aggr-Core)=8*4*2=64 (示例)
  run ("fat_tree", "allreduce",
       EnergyInventory::FatTree (/*N*/128, /*nTor*/32, /*nAggr*/32, /*nCore*/16,
                                 /*L2*/128, /*L3*/64, /*rate*/100, /*gpuPerSrv*/1),
       T, LB, DLV, PKT, /*tput*/ 0.8 * (128 * 100 / 2.0));

  // ── 2) Leaf-Spine N=512, nLeaf=16, nSpine=32, L2=16*32=512 ──
  run ("leaf_spine", "allreduce",
       EnergyInventory::LeafSpine (512, 16, 32, 512, /*rate*/100, 1),
       T, LB, DLV, PKT, 0.8 * (512 * 100 / 2.0));

  // ── 3) Spectrum-X N=1332, nLeaf=42, nSpine=32, L2=42*32=1344, 400G ──
  run ("spectrum_x", "allreduce",
       EnergyInventory::LeafSpine (1332, 42, 32, 1344, 400, 1),
       T, LB, DLV, PKT, 0.8 * (1344 * 400 / 2.0));

  // ── 4) Rail-IB R=4, 单 rail N=333, nLeaf=21, nSpine=16, L2=21*16=336, 400G ──
  run ("rail_ib", "allreduce",
       EnergyInventory::Rail (4, 333, 21, 16, 336, 400),
       T, LB, DLV, PKT, 0.8 * (4 * 336 * 400 / 2.0));

  // ── 5) Dragonfly N=1332, router=222, intra=555, global=666, 400G ──
  run ("dragonfly_ib", "allreduce",
       EnergyInventory::Dragonfly (1332, 222, 555, 666, 400, 1),
       T, LB, DLV, PKT, 0.8 * (666 * 400 / 2.0));

  // ── 6) 无交换机 3D torus N=1331 (11^3), 你的方案：每节点(GPU)4 FPGA ──
  //   3D torus degree=6 → 链路数=1331*6/2=3993；对分横切链路=2*11^2=242
  run ("mesh3d_switchless", "allreduce",
       EnergyInventory::Switchless (/*nNodes*/1331, /*nLinks*/3993,
                                    /*fpgaPerNode*/4, /*rate*/400,
                                    /*opticalFrac*/1.0, /*bisectionLinks*/242),
       T, LB, DLV, PKT, 0.8 * (242 * 400));

  // ── 6b) 同上但短直连改用 DAC 铜（光占比 30%）——展示介质对能耗的杠杆 ──
  run ("mesh3d_switchless", "allreduce_DACheavy",
       EnergyInventory::Switchless (1331, 3993, 4, 400, /*opticalFrac*/0.30,
                                    /*bisectionLinks*/242),
       T, LB, DLV, PKT, 0.8 * (242 * 400));

  std::printf ("\n[ok] wrote energy_unified.csv\n");
  return 0;
}
