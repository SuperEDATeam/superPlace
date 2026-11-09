#!/usr/bin/env bash
# 等 RePlAce 的 run_exp.sh / build_exp/replace 退出后，用当前源码的 Release 构建
# 跑 BookShelf benchmark 的 init+gp。target_density 与 RePlAce run_exp.sh 的竞赛表一致。
#
# 覆盖：
#   benchmarks/ispd2005  8 个，密度 1.00
#   benchmarks/ispd2006  8 个，密度 0.50 / 0.80 / 0.90
#   benchmarks/mms       16 个，按同名设计套用上表
# 不覆盖 ispd2005free（原版的派生副本）和 ispd2015（LEF/DEF，没有 .aux）。
#
# 可续跑：results/density_batch/wall.csv 里已是 OK 的设计会跳过。
set -u
cd "$(dirname "$0")/.."

BENCH_ROOT=${BENCH_ROOT:-/home/cai/EDA/GlobalPlacement/benchmarks}
OUT=${OUT:-results/density_batch}
CFG_BASE=${CFG_BASE:-configs/default.json}
BIN=build/release/superplace-cli
QUIET_SEC=${QUIET_SEC:-180}

mkdir -p "$OUT/cfg" "$OUT/logs"
log() { printf '%s %s\n' "$(date '+%F %T')" "$*" | tee -a "$OUT/driver.log"; }

replace_busy() {
    # 方括号避免 pgrep 匹配到自己的命令行
    pgrep -f '[b]uild_exp/replace|[r]un_exp\.sh' >/dev/null
}

log "driver start pid=$$  waiting until RePlAce has been idle for ${QUIET_SEC}s"
quiet=0
while true; do
    if replace_busy; then
        quiet=0
        log "RePlAce still running: $(pgrep -af '[b]uild_exp/replace|[r]un_exp\.sh' | head -1)"
        sleep 60
        continue
    fi
    quiet=$((quiet + 60))
    if [ "$quiet" -ge "$QUIET_SEC" ]; then
        break
    fi
    log "RePlAce idle ${quiet}s / ${QUIET_SEC}s"
    sleep 60
done
log "RePlAce idle, building Release"

cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_CUDA=OFF -DENABLE_GUI=OFF >> "$OUT/driver.log" 2>&1
cmake --build build/release --target superplace-cli >> "$OUT/driver.log" 2>&1
if [ ! -x "$BIN" ]; then
    log "build failed, no $BIN"
    exit 1
fi
log "build ok: $BIN"

density_of() {
    case "$1" in
        adaptec1|adaptec2|adaptec3|adaptec4|bigblue1|bigblue2|bigblue3|bigblue4) echo 1.00 ;;
        adaptec5|newblue4|newblue5) echo 0.50 ;;
        newblue1|newblue3|newblue6|newblue7) echo 0.80 ;;
        newblue2) echo 0.90 ;;
        *) echo "" ;;
    esac
}

suite_of() {
    case "$1" in
        adaptec1|adaptec2|adaptec3|adaptec4|bigblue1|bigblue2|bigblue3|bigblue4) echo ispd2005 ;;
        adaptec5|newblue1|newblue2|newblue3|newblue4|newblue5|newblue6|newblue7) echo ispd2006 ;;
        *) echo "" ;;
    esac
}

# 规模从小到大，与 scripts/run_audit.sh 相同。
DESIGNS=(
    adaptec1 adaptec2 bigblue1 newblue1 newblue2 adaptec3 newblue3
    adaptec4 bigblue2 newblue4 adaptec5 bigblue3 newblue5 newblue6
    bigblue4 newblue7
)

wall="$OUT/wall.csv"
[ -f "$wall" ] || echo "suite,design,target_density,status,wall_s,peak_rss_mb" > "$wall"

write_cfg() {
    local design=$1 den=$2 dst=$3
    python3 - "$CFG_BASE" "$dst" "$den" <<'PY'
import json, sys
src, dst, den = sys.argv[1:]
with open(src, encoding="utf-8") as f:
    cfg = json.load(f)
cfg["target_density"] = float(den)
with open(dst, "w", encoding="utf-8") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
PY
}

run_one() {
    local suite=$1 design=$2 den=$3
    local key="${suite}/${design}"
    local aux="${BENCH_ROOT}/${suite}/${design}/${design}.aux"
    if [ ! -f "$aux" ]; then
        log "SKIP $key (no $aux)"
        return
    fi
    if grep -q "^${suite},${design},.*,OK," "$wall" 2>/dev/null; then
        log "DONE $key (already OK)"
        return
    fi

    local cfg="$OUT/cfg/${design}.json"
    write_cfg "$design" "$den" "$cfg"
    local outdir="$OUT/$suite"
    local logfile="$OUT/logs/${suite}_${design}.log"
    mkdir -p "$outdir"

    log "START $key density=$den"
    local t0 t1 wall_s rss mb st rc
    t0=$(date +%s.%N)
    /usr/bin/time -f "%M" -o "$OUT/logs/${suite}_${design}.rss" \
        "$BIN" --aux "$aux" --stage init,gp --config "$cfg" \
        --no-plot --out "$outdir" > "$logfile" 2>&1
    rc=$?
    t1=$(date +%s.%N)
    wall_s=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b-a}')
    rss=$(cat "$OUT/logs/${suite}_${design}.rss" 2>/dev/null || echo 0)
    mb=$(awk -v k="$rss" 'BEGIN{printf "%d", k/1024}')
    if [ "$rc" = 0 ]; then st=OK; else st="FAIL($rc)"; fi
    printf '%s,%s,%s,%s,%s,%s\n' "$suite" "$design" "$den" "$st" "$wall_s" "$mb" >> "$wall"
    log "END   $key $st  ${wall_s}s  ${mb}MB"
}

for design in "${DESIGNS[@]}"; do
    den=$(density_of "$design")
    suite=$(suite_of "$design")
    [ -n "$den" ] && [ -n "$suite" ] || { log "unknown design $design"; continue; }
    run_one "$suite" "$design" "$den"
    run_one "mms" "$design" "$den"
done

log "batch finished"
echo "DONE" > "$OUT/.done"
