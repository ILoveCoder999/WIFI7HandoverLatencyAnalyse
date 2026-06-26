#!/usr/bin/env bash
# =============================================================================
# run_experiments.sh
#   复现论文 "A Scalable, Commodity Data Center Network Architecture" 中
#   Table 2 的 "Two-Level Table" 一列 (Stride 与 Random 测试)。
#
# 用法 (在 ns-3 根目录下执行):
#   bash scratch/fattree/run_experiments.sh
#
# 可用环境变量覆盖默认值:
#   K=4          胖树端口数 (偶数)
#   SIMTIME=10   每次仿真时长(秒)；想更接近论文可设 60
#   RUNS=5       Random 测试取平均的随机种子个数
#
# 例: K=4 SIMTIME=20 RUNS=10 bash scratch/fattree/run_experiments.sh
# =============================================================================
set -u

K=${K:-4}
SIMTIME=${SIMTIME:-10}
RUNS=${RUNS:-5}

# 必须能在当前目录找到 ns3 脚本
if [ ! -x ./ns3 ]; then
  echo "错误: 请在 ns-3 根目录(含 ./ns3 脚本)下运行本脚本。" >&2
  exit 1
fi

# 跑一次仿真并抽取 RESULT 行里的百分比
runpct () {
  ./ns3 run "fattree $*" 2>/dev/null | awk -F, '/^RESULT,/{print $5}'
}

echo "k=$K, simTime=${SIMTIME}s, Random 平均次数=${RUNS}"
echo "-------------------------------------------------------------"
printf "%-16s %18s %16s\n" "Pattern" "本仿真(Two-Level)" "论文(Two-Level)"
echo "-------------------------------------------------------------"

# ---- Stride(1,2,4,8): 论文中 Two-Level 均为 100% ----
for s in 1 2 4 8; do
  p=$(runpct --pattern=stride --stride=$s --k=$K --simTime=$SIMTIME)
  printf "%-16s %16s %% %14s %%\n" "Stride($s)" "${p:-FAIL}" "100.0"
done

# ---- Random: 多个种子取平均，论文中 Two-Level = 75.0% ----
total=0
ok=0
for seed in $(seq 1 "$RUNS"); do
  p=$(runpct --pattern=random --seed=$seed --k=$K --simTime=$SIMTIME)
  if [ -n "$p" ]; then
    echo "    random seed=$seed -> $p %"
    total=$(awk -v a="$total" -v b="$p" 'BEGIN{print a+b}')
    ok=$((ok + 1))
  fi
done
if [ "$ok" -gt 0 ]; then
  avg=$(awk -v t="$total" -v n="$ok" 'BEGIN{printf "%.1f", t/n}')
else
  avg="FAIL"
fi
echo "-------------------------------------------------------------"
printf "%-16s %16s %% %14s %%\n" "Random(avg$ok)" "$avg" "75.0"
echo "-------------------------------------------------------------"
echo "提示: Stride 因 UDP 包头开销通常略低于 100%；Random 在 ~75% 上下波动属正常。"
