# 统一功耗 / 能耗模型 — 接入说明

一套参数、一个公式，套到你全部拓扑上，让"吞吐 vs 能耗"对比是 apples-to-apples 的。

## 文件

| 文件 | 作用 |
|---|---|
| `energy-model.h` | 唯一真值来源。功耗参数表 + 计算引擎 + 各拓扑清单构造器。所有 `*-sim.cc` 都 `#include` 它。 |
| `test_energy.cc` | 独立验证（不需 ns-3）。用各拓扑真实规模跑一遍，打印摘要、写统一 CSV。也是接入示例。 |
| `aggregate_energy.py` | 读 `energy_unified.csv`，出对比表 + 三张图（总功耗 / 每 GPU 功耗 / 网络能效）。 |
| `energy_unified.csv` | 所有拓扑所有场景追加写同一个文件，一行一次运行。 |

## 模型怎么算的

功耗拆到**可计数的物理单元**，分静态（常开）+ 动态（随转发比特）：

```
静态功耗 P_static =
    交换机:  nSwitch × switchChassisW  +  nSwitchPorts × asicPerPortW × rateScale
  + SerDes:  (所有端口) × serdesPerPortW × rateScale
  + 光模块:  nOpticalPorts × xcvrW(rate)        # 其余端口按 DAC 计
  + NIC:     nNic × nicBaseW × rateScale
  + FPGA:    nFpga × fpgaBaseW                  # 无交换机架构在这里付钱

动态能耗 E_dyn = 全网链路发送比特 × dynamicPjPerBit
总能耗  E_total = P_static × 时长 + E_dyn
```

为什么以静态为主：ElasticTree (NSDI'10) 实测交换机空载≈满载 70%，光模块/激光器恒定常开 —— 网络器件**非能量正比**。所以结果里 `staticFrac` 接近 100% 是符合实测的，不是 bug。

**核心对比指标**：`pJ_per_delivered_bit`（每交付比特的网络能耗，越低越好）、`perGpuW`（每 GPU 网络功耗）。

## 参数表（在 `energy-model.h` 顶部 `EnergyParams`，改这里=改全部对比）

| 参数 | 默认 | 出处 / 可调区间 |
|---|---|---|
| `switchChassisW` | 250 W | 风扇+CPU+PSU+ASIC静态；ElasticTree NSDI'10 [150~400] |
| `asicPerPortW` | 4 W | 51.2T ASIC(~500W)/128口反推 [2~6] |
| `nicBaseW` | 20 W | ConnectX-7 400G [15~25]；BF-3 DPU≈75~150 |
| `fpgaBaseW` | 50 W | Alveo/U系列 FPGA 网卡 [40~75] |
| `serdesPerPortW` | 1.5 W | SerDes 占 ASIC>30%，Abts ISCA'10 [1.0~2.5] |
| `xcvr400gW` / `xcvr800gW` | 10 / 16 W | 400G≈8~12W，800G≈14~20W（厂商） |
| `dacW` | 0.3 W | 无源 DAC≈0；有源 AEC≈0.5~1.5 |
| `dynamicPjPerBit` | 3 pJ/bit | 网络非能量正比→动态占比小 [1~20] |

命令行整体覆盖任一参数即可做敏感性扫描（sensitivity sweep）。

## 公平对比（apples-to-apples）层

能耗只有在"同等配置"下才可比。直连架构和 Clos 不是一个维度的东西，所以模型为
每个拓扑算出**配置预算**并按"每 GPU"归一化，把"你只是堆了更多 NIC"显形：

| 指标 | 含义 | 方向 |
|---|---|---|
| `bisecPerGpuGbps` | 每 GPU 分到多少跨网（对分）带宽 | 越高越好 |
| `portsPerGpu` / `cablesPerGpu` / `xcvrPerGpu` | 每 GPU 用了多少端口/线缆/光模块 | 越低越省 |
| `wPerBisecGbps` | 配单位对分带宽要花多少瓦（性价比） | 越低越好 |
| `bisecUtil` | 实测吞吐 ÷ 配置对分带宽（利用率） | 接近 1 = 配置不浪费 |
| `gbpsPerW` | 每瓦吞吐（真能效） | 越高越好 |

**对分带宽公式**（各构造器自动算，可 `em.inv.bisectionGbps=...` 覆盖）：

| 拓扑 | bisection (Gbps) | 说明 |
|---|---|---|
| FatTree（非阻塞3层） | `N·R/2` | 满对分 |
| LeafSpine / Spectrum-X | `L2·R/2` | 受 spine 链路数限（L2=leaf-spine 链路数） |
| Rail（R 条） | `R·(L2_rail·R/2)` | R 条 rail 对分之和 |
| Dragonfly | `Lglobal·R/2` | 受全局链路限（估计值） |
| 无交换机 mesh/torus | `bisectionLinks·R` | d 维 torus 边长 k：横切链路=`2·k^(d-1)`；mesh=`k^(d-1)` |

> 这就回应了"对齐节点数还不够"：rail 给每服务器 R 块 HCA（注入带宽×R），但
> 任意跨 GPU 通信要走 NVLink；mesh 度高=注入/转发口多但对分反而低。把这些
> 都摆进 per-GPU 列，吞吐高低就不会被误读成"网络更好"。

**默认参数下的公平对比**（1332-GPU/400G 同档位最可比，按性价比排序）：

| 拓扑 | bisec/GPU | ports/GPU | xcvr/GPU | W/Gbps-bis | Gbps/W |
|---|---:|---:|---:|---:|---:|
| spectrum_x | 201.8 | 4.0 | 4.0 | **0.46** | **1.75** |
| rail_ib | 201.8 | 4.0 | 4.0 | 0.53 | 1.52 |
| dragonfly_ib | 100.0 | 3.8 | 3.8 | 1.17 | 0.68 |
| mesh3d 无交换机(DAC) | 72.7 | 6.0 | 1.8 | 3.14 | 0.26 |
| mesh3d 无交换机(全光) | 72.7 | 6.0 | 6.0 | 3.70 | 0.22 |

读法：当前默认参数下，无交换机 3D torus **每 GPU 对分带宽更低**（72.7 vs 201.8）却
**每 GPU 端口/光模块更多**（6 vs 4）、**配带宽性价比差 ~8×**。要让直连方案翻盘，得
靠：低维 torus 控制度数、短直连大量用 DAC、FPGA 兼做计算摊薄功耗、或流量局部性
高到根本不需要满对分（`bisecUtil`<<1 时直连的低对分不吃亏）。模型让这些权衡量化。

接入时多一行 `em.SetThroughput(tputGbps);` 即可激活 `bisecUtil`/`gbpsPerW`。

## 接入：交换式拓扑（fat-tree / leaf-spine / spectrum-x / dragonfly / rail）

这些 sim 已经用 `MacTx` trace 累加 `g_total_bits`，并算了 `rxDurSec`、`delivered`。
在 `main()` 末尾（统计完吞吐后）加 6 行。各拓扑只有"清单那一行"不同：

```cpp
#include "energy-model.h"          // 文件顶部

// ... 仿真与吞吐统计之后 ...
EnergyModel em;
em.SetBits (g_total_bits);                 // 全网链路发送总比特
em.SetDuration (rxDurSec);                 // 稳态收包时长(秒)
em.SetTraffic (delivered, pktBytes);       // 每交付比特能耗
em.SetThroughput (tputGbps);               // 实测吞吐 → 激活 bisecUtil/gbpsPerW
em.inv = /* 见下表，对应你的拓扑 */;
em.PrintSummary ("fat_tree", scenario);
em.WriteCsv ("energy_unified.csv", "fat_tree", scenario);
```

各拓扑清单一行（`rate = lineRateBps/1e9`，按你常量算 L2/L3）：

```cpp
// fat_tree   K=8: nTor=32,nAggr=32,nCore=16, L2=8*4*4=128, L3=8*4*2=64
em.inv = EnergyInventory::FatTree (N, 32, 32, 16, 128, 64, lineRateBps/1e9, /*gpuPerSrv*/1);

// leaf_spine  nLeaf=16,nSpine=32, L2=nLeaf*nSpine=512
em.inv = EnergyInventory::LeafSpine (N, 16, 32, 512, lineRateBps/1e9, 1);

// spectrum_x  N=1332,nLeaf=42,nSpine=32, L2=42*32=1344, 400G
em.inv = EnergyInventory::LeafSpine (1332, 42, 32, 1344, 400, 1);

// rail_ib     R=4, 单rail N=333,nLeaf=21,nSpine=16, L2=21*16=336, 400G
em.inv = EnergyInventory::Rail (4, 333, 21, 16, 336, 400);

// dragonfly_ib  N=1332,router=222,intra=555,global=666, 400G
em.inv = EnergyInventory::Dragonfly (1332, 222, 555, 666, 400, 1);
```

> 一致性提示：把 `nGpu` 的口径在所有拓扑里统一（要么每端点=1 个 GPU，要么乘以每服务器 GPU 数）。默认按"每端点=1 GPU"，这样 1332 端点的几套方案直接可比。

## 接入：无交换机直连（mesh2d / mesh3d / mesh4d）

关键是**如实数清单**，别让"去交换机"白占便宜。两步：

**1) 在建链处数链路条数**（`addEdge` lambda 里加一行）：

```cpp
static uint64_t g_link_count = 0;
auto addEdge = [&](uint32_t u, uint32_t v){
    if (v <= u) return;
    /* ... p2p.Install ... */
    g_link_count++;                 // ← 加这行
};
```

**2) `main()` 末尾**（`g_total_bits_transmitted` 已由 MacTx 累加；2D 老版本若没有，照 mesh3d 加上 MacTx trace）：

```cpp
#include "energy-model.h"
EnergyModel em;
em.SetBits (g_total_bits_transmitted);
em.SetDuration (rxDurSec);                 // 没有就用 (g_last-g_first)/1e9
em.SetTraffic (delivered, pktBytes);
em.SetThroughput (tputGbps);
em.inv = EnergyInventory::Switchless (
            /*nNodes*/ N,
            /*nLinks*/ g_link_count,        // ← 用实测链路数，最可靠
            /*fpgaPerNode*/ 4,              // 每 GPU 4 块 FPGA（你的方案）
            /*rate*/ 400,
            /*opticalFrac*/ 1.0,            // 短直连可改 DAC，见下
            /*bisectionLinks*/ 2*11*11);    // 3D torus(11^3) 横切链路=2·k^(d-1)
em.PrintSummary ("mesh3d_switchless", scenario);
em.WriteCsv ("energy_unified.csv", "mesh3d_switchless", scenario);
```

两个会被审稿人抓的点，模型已替你暴露：

- **FPGA 是大头**：每节点 4 块 FPGA × 50 W = 200 W/节点，直接进 `nFpga`。这是"去交换机"的真实代价。
- **端口/光模块随直连度暴增**：`nLinks` 越大、每端口一个光模块，功耗越高。你现在的 mesh3d 若是"扁平 HyperX"（每维全连接，度=3·(M-1)=30），每节点要 30 个端口 —— 远超 4 块 FPGA 能提供的端口数，物理不可行。若是真正的 **3D torus**（度=6），`nLinks≈N×3`，才现实。`nLinks` 用实测值能让这个矛盾显形。
- **介质杠杆**：短直连用 DAC 铜（`opticalFrac<1`）能显著降功耗。示例里 `opticalFrac` 从 1.0→0.30，每 GPU 从 269→228 W。

## 跑通 + 出图

```bash
g++ -std=c++14 -O2 test_energy.cc -o test_energy && ./test_energy   # 验证+写CSV
python3 aggregate_energy.py energy_unified.csv --plot               # 对比表+三张图
```

> 注意：`WriteCsv` 是**追加**写（方便累积多次运行/多场景）。想从头来，先删 `energy_unified.csv`。

## 当前默认参数下的代表性结果（占位吞吐，仅看相对关系）

| 拓扑 | 交换机 | FPGA | 每GPU功耗 | 网络能效(pJ/bit) |
|---|---:|---:|---:|---:|
| leaf_spine (100G) | 48 | 0 | 5.4 W | 1346 |
| fat_tree (100G) | 80 | 0 | 22.5 W | 1408 |
| spectrum_x (400G) | 74 | 0 | 92.2 W | 7498 |
| rail_ib (400G) | 148 | 0 | 106.1 W | 8627 |
| dragonfly_ib (400G) | 222 | 0 | 117.1 W | 9523 |
| mesh3d 无交换机 (DAC重) | 0 | 5324 | 228.3 W | 18547 |
| mesh3d 无交换机 (全光) | 0 | 5324 | 269.1 W | 21858 |

绝对值取决于真实吞吐与参数；换上你 sim 的实测 `g_total_bits` / `delivered` 即得真实对比。

## 参数出处

- Abts et al., *Energy Proportional Datacenter Networks*, ISCA 2010.
- Heller et al., *ElasticTree: Saving Energy in Data Center Networks*, NSDI 2010.
- 400G/800G 可插光模块与 DAC 功耗：厂商 / 行业公开数据。
- NVIDIA Spectrum-4 / ConnectX-7 / BlueField-3：厂商规格量级。
