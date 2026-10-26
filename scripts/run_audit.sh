#!/usr/bin/env bash
# ISPD2005 / ISPD2006 全量审计：逐设计跑 init+gp，记录耗时、峰值内存与各阶段指标。
#
# 按规模从小到大排，先拿到小设计的结果；大设计万一出问题也不至于堵住全部。
set -u
cd "$(dirname "$0")/.."

OUT=${1:-results/audit}
CFG=configs/default.json
mkdir -p "$OUT"

# 规模升序：对象数见 work_report/04 §6.3。可用环境变量 DESIGNS 覆盖，
# 例如 DESIGNS="mms/adaptec1 mms/adaptec2" scripts/run_audit.sh results/audit_mms
DESIGNS=${DESIGNS:-"
ispd2005/adaptec1
ispd2005/adaptec2
ispd2005/bigblue1
ispd2006/newblue1
ispd2006/newblue2
ispd2005/adaptec3
ispd2006/newblue3
ispd2005/adaptec4
ispd2005/bigblue2
ispd2006/newblue4
ispd2006/adaptec5
ispd2005/bigblue3
ispd2006/newblue5
ispd2006/newblue6
ispd2005/bigblue4
ispd2006/newblue7
"}

# 可续跑：已经成功跑完的设计直接跳过。WSL 虚拟机会不定期重启，全量一趟要一个多小时，
# 断一次就从头来是不可接受的。
[ -f "$OUT/wall.csv" ] || echo "design,status,wall_s,peak_rss_mb" > "$OUT/wall.csv"

for p in $DESIGNS; do
    suite=${p%%/*}; d=${p##*/}
    aux="benchmarks/$suite/$d/$d.aux"
    [ -f "$aux" ] || { echo "SKIP $p (缺 $aux)"; continue; }
    if grep -q "^$p,OK," "$OUT/wall.csv" 2>/dev/null; then
        echo "DONE $p（已有结果，跳过）"; continue
    fi

    log="$OUT/$d.log"
    t0=$(date +%s.%N)
    rss=$(/usr/bin/time -f "%M" -o "$OUT/$d.rss" \
          ./build_clean/superplace-cli --aux "$aux" --stage init,gp \
          --config "$CFG" --no-plot --out "$OUT" > "$log" 2>&1; cat "$OUT/$d.rss" 2>/dev/null)
    rc=$?
    t1=$(date +%s.%N)
    wall=$(echo "$t1 - $t0" | bc)
    mb=$(echo "scale=0; ${rss:-0}/1024" | bc)
    st=$([ "$rc" = 0 ] && echo OK || echo "FAIL($rc)")
    printf "%-22s %-10s %8.1f s  %6s MB\n" "$p" "$st" "$wall" "$mb"
    printf "%s,%s,%.1f,%s\n" "$p" "$st" "$wall" "$mb" >> "$OUT/wall.csv"
done

echo "DONE" > "$OUT/.done"
