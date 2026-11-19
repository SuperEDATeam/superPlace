#!/usr/bin/env bash
# 黄金值跨环境探测：记录编译与运行环境，用几种指令集选项分别构建 superplace-cli，
# 在三个黄金设计上跑 init,gp，输出 .pl 的 SHA256。把生成的报告发给对方逐行比对。
#
# 用法（仓库根目录）：scripts/golden_env_probe.sh [--quick]
#   --quick 只跑 thin1（只走 QP，几秒钟），否则还跑 adaptec1 与 MMS adaptec1（各约半分钟）
# 报告：results/golden_env/probe-<机器名>.txt
#
# 背景见 work_report/07 §4.7：GCC 默认 -ffp-contract=fast，会按目标 CPU 的调优
# 把 a*b+c 合成 FMA，而 ENABLE_NATIVE=ON 时各机器的 -march=native 不同，结果最后几位随之不同。
set -eu
cd "$(dirname "$0")/.."

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1
OUT=results/golden_env
mkdir -p "$OUT"
REPORT="$OUT/probe-$(hostname).txt"
CONFIG=configs/m3/golden.json
THREADS=8

has() { grep -qw "$1" /proc/cpuinfo && echo 有 || echo 无; }
{
    echo "# 黄金值跨环境探测 $(date '+%F %T')"
    echo "机器名    $(hostname)"
    echo "源码提交  $(git rev-parse --short HEAD)$(git diff --quiet -- src CMakeLists.txt || echo '（src 有未提交改动）')"
    echo "CPU       $(lscpu | sed -n 's/^Model name:\s*//p')"
    echo "指令集    avx2=$(has avx2) fma=$(has fma) avx512f=$(has avx512f)"
    echo "native    $(gcc -march=native -Q --help=target 2>/dev/null | awk '$1=="-march="{print $2}')"
    echo "系统      $(lsb_release -ds 2>/dev/null || uname -sr)"
    echo "GCC       $(gcc -dumpfullversion)"
    echo "glibc     $(ldd --version | head -1 | awk '{print $NF}')"
    echo "FFTW3f    $(pkg-config --modversion fftw3f 2>/dev/null || echo 未知)"
    echo "Eigen     $(awk '/define EIGEN_(WORLD|MAJOR|MINOR)_VERSION/{printf "%s.", $3}' /usr/include/eigen3/Eigen/src/Core/util/Macros.h 2>/dev/null | sed 's/\.$//')"
    echo
    echo "# 变体 设计 init.pl gp.pl"
} > "$REPORT"

# 名称 ENABLE_NATIVE 额外 CXX 选项
# 本机（i9-14900HX，alderlake）2026-11-15 的参照哈希见 work_report/07 附录 C。
# 关闭 FMA 合并的两个变体（*-nocontract）在 AVX2 机器上应与参照逐字节相同。
VARIANTS=(
    "native ON "
    "x86-64-v3 OFF -march=x86-64-v3"
    "native-nocontract ON -ffp-contract=off"
    "v3-nocontract OFF -march=x86-64-v3 -ffp-contract=off"
)
DESIGNS=("thin1 test_data/thin1/thin1.aux")
if [ "$QUICK" = 0 ]; then
    DESIGNS+=("adaptec1 test_data/adaptec1/adaptec1.aux" "mms_adaptec1 test_data/mms_adaptec1/adaptec1.aux")
fi

for v in "${VARIANTS[@]}"; do
    read -r name native flags <<< "$v"
    b="build_probe/$name"
    echo "构建 $name ..."
    cmake -S . -B "$b" -DCMAKE_BUILD_TYPE=Release -DENABLE_NATIVE="$native" \
          -DCMAKE_CXX_FLAGS="${flags:-}" > "$OUT/probe-$name.cfg.log" 2>&1
    cmake --build "$b" --target superplace-cli -j"$(nproc)" > "$OUT/probe-$name.build.log" 2>&1
    for d in "${DESIGNS[@]}"; do
        read -r design aux <<< "$d"
        [ -f "$aux" ] || { echo "缺少 $aux（先在 build 目录跑一次 cmake 配置以解压测试数据）"; exit 1; }
        n=$(basename "$aux" .aux)
        o="$OUT/probe-run/$name/$design"
        rm -rf "$o"
        mkdir -p "$(dirname "$o")"
        echo "  运行 $design"
        OMP_NUM_THREADS=$THREADS "$b/superplace-cli" --aux "$aux" --stage init,gp --config "$CONFIG" \
            --threads $THREADS --no-plot --out "$o" > "$o.log" 2>&1
        # 输出完整 64 位哈希：严格判断两台机器逐字节一致需要全长，前缀只能用于快速浏览
        printf "%-20s %-13s %s %s\n" "$name" "$design" \
            "$(sha256sum "$o/$n/$n.init.pl" | cut -c1-64)" "$(sha256sum "$o/$n/$n.gp.pl" | cut -c1-64)" >> "$REPORT"
    done
done

echo
cat "$REPORT"
echo
echo "报告已写入 $REPORT"
