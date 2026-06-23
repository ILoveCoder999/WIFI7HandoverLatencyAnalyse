# 289-host 2D mesh + DOR routing + credit flow control (ns-3 包)

主机粒度建模:每台主机 = 一个节点(8卡×4端口=32端口当整体)。
17×17 = 289 台主机的 2D mesh,每台连同行 16 + 同列 16 = 32 度。
任意两台主机最多 1 跳中转(同行/同列直达 0 跳,跨行列经交点 1 跳)。
289 × 8 = 2312 GPU。

## 文件

| 文件 | 作用 |
|------|------|
| `mesh-sim.cc` | 主仿真:289 台 2D mesh + DOR 路由 + credit 流控 + 三种压测场景 |
| `mesh-route-header.h` | DOR 源路由包头(最多 3 个节点:源/交点/目标) |
| `mesh-credit.h` | 每目的地 credit 流控(抗 incast 无损) |
| `CMakeLists.txt` | scratch 子目录构建定义 |
| `setup_and_build.sh` | 一键拷入 ns-3 并编译 |

## 一键编译

```bash
git clone https://gitlab.com/nsnam/ns-3-dev.git
./setup_and_build.sh /path/to/ns-3-dev
```

## 运行

```bash
cd /path/to/ns-3-dev

# 连通性/跳数 sanity:确认任意对 <=1 跳中转
./ns3 run "mesh-sim --scenario=uniform"

# incast 抗突发对比(方案核心):
./ns3 run "mesh-sim --scenario=incast --credit=1 --fanin=64"   # 有流控,无损
./ns3 run "mesh-sim --scenario=incast --credit=0 --fanin=64"   # 无流控,丢包

# 热点:多源打 2x2 角落,看 mesh 交点负载
./ns3 run "mesh-sim --scenario=hotspot --credit=1"
```

对比两次 incast 的 `delivered` 和 `dropped`:credit=1 应零丢包(背压排队),
credit=0 在 fan-in 超过 buffer 时大量丢包。

## 参数

| 参数 | 含义 | 默认 |
|------|------|------|
| `--scenario` | uniform / incast / hotspot | incast |
| `--credit` | 1=credit 流控, 0=无流控 | 1 |
| `--fanin` | incast 扇入(发送方数) | 64 |
| `--pktBytes` | 包字节 | 1024 |
| `--bufferPkts` | 接收 buffer 深度(包) | 64 |
| `--linkRate` / `--linkDelay` | 链路速率/时延 | 100Gbps / 200ns |

## 拓扑与路由(已本机验证)

- 17×17 mesh:289 主机,4624 链路,度数严格 32,直径 2。
- DOR 行优先路由:同行→直达;同列→直达;否则经 (源行, 目标列) 交点。
- 对全部 83232 对主机验证:最多 1 跳中转,零无效跳(见 mesh_logic_check.py)。

## 设计说明:为什么用 DOR 而不是 Valiant

本机仿真(mesh_routing_logic.py)显示:在"行列全连接"的 2D mesh 上,DOR 已
天然均衡(转置流量下 imbalance 1.0x),Valiant 随机绕路反而放大总流量、增加
负载和延迟。Valiant 是为长直径、链路稀缺的标准 mesh/Dragonfly 设计的;本拓扑
每维一跳直达,不需要它。incast 热点(目的地拥塞)由 credit 流控解决,不是路由。

## 验证状态

- 拓扑构造 + DOR 路由:本机独立编译运行验证(83232 对全对、零错误)。
- credit 流控逻辑:与前一个 OCS 包同款机制,本机离散事件验证过抗 incast 无损。
- ns-3 类型相关代码:由真实 ns-3 的 CMake 编译保证(本机无 ns-3 树)。
  若遇 API 版本差异(3.38 vs 3.43),按报错微调即可。
