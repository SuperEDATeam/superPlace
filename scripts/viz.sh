#!/usr/bin/env bash
# 一条命令启动布局查看器。会自动做这几件事：
#   - 增量构建前端（已是最新则几乎不花时间）
#   - 在 WSL2 下设置 GALLIUM_DRIVER=d3d12 打开显卡加速（原生 Linux 不设）
#   - 不依赖当前目录，在哪里运行都行
#   - 指定一个设计时，若还没录过，先用 CLI 录制，再打开
#
# 用法：
#   scripts/viz.sh                          打开仓库自带的合成样本
#   scripts/viz.sh newblue1                 按设计名查找；没录过就先录（约 45 秒）
#   scripts/viz.sh mms/adaptec1             同名设计有多份时，用 套件/设计 指定
#   scripts/viz.sh path/to/design.aux       指定 .aux 文件
#   scripts/viz.sh path/to/viz              直接打开一个已有的 bundle 目录
#
#   --rerecord     即使录过也重新录（算法代码改过之后用）
#   --stride N     每 N 轮迭代录一帧，默认 10
#   其余参数原样交给查看器，例如 --frame -1、--compare
#
# 录制结果存在 results/play/<套件>/<设计>/viz/，按套件分开，避免同名设计互相覆盖。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STRIDE=10
RERECORD=0
TARGET=""
PASS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rerecord) RERECORD=1; shift ;;
        --stride)   STRIDE="${2:?--stride 需要一个数字}"; shift 2 ;;
        -h|--help)  sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)
            # 查看器参数；带值的那几个要连同它的值一起转交
            case "$1" in
                --frame|--bench|--screenshot|--shot-after) PASS+=("$1" "${2:?$1 需要一个值}"); shift 2 ;;
                *) PASS+=("$1"); shift ;;
            esac ;;
        *)
            if [[ -z "$TARGET" ]]; then TARGET="$1"; else PASS+=("$1"); fi
            shift ;;
    esac
done

say() { printf '\033[36m[viz]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31m[viz]\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 前端：增量构建
if [[ ! -f "$ROOT/gui/build/CMakeCache.txt" ]]; then
    say "首次构建前端（约 1~2 分钟）"
    cmake -S "$ROOT/gui" -B "$ROOT/gui/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
fi
cmake --build "$ROOT/gui/build" -j"$(nproc)" --target superplace-viz >/dev/null \
    || die "前端构建失败，手动运行 cmake --build gui/build 查看错误"

# ---- 解析要打开的东西
record_and_resolve() {   # $1 = .aux 路径, $2 = 套件名；输出 bundle 目录
    local aux="$1" suite="$2" name out bundle
    name="$(basename "$aux" .aux)"
    out="$ROOT/results/play/$suite"
    bundle="$out/$name/viz"
    if [[ $RERECORD -eq 1 || ! -f "$bundle/scene.json" ]]; then
        say "录制 $suite/$name（每 $STRIDE 轮一帧）……"
        cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build "$ROOT/build" -j"$(nproc)" --target superplace-cli >/dev/null \
            || die "算法侧构建失败，手动运行 cmake --build build 查看错误"
        "$ROOT/build/superplace-cli" --aux "$aux" --config "$ROOT/configs/default.json" \
            --stage parse,init,gp --frame-stride "$STRIDE" --no-plot --out "$out" \
            2>&1 | grep -E "gp done|viz bundle|error|ERROR" >&2 || true
        [[ -f "$bundle/scene.json" ]] || die "录制失败：没有生成 $bundle"
    else
        say "使用已录好的 $suite/$name（加 --rerecord 重新录）"
    fi
    printf '%s\n' "$bundle"
}

if [[ -z "$TARGET" ]]; then
    BUNDLE="$ROOT/gui/sample-data/synthetic"
elif [[ -d "$TARGET" && -f "$TARGET/scene.json" ]]; then
    BUNDLE="$(cd "$TARGET" && pwd)"
elif [[ "$TARGET" == /* || "$TARGET" == .* || "$TARGET" == ~* ]] && [[ "$TARGET" != *.aux ]]; then
    # 明显是路径的参数不要再当设计名去查，否则报错会变成莫名其妙的"没有 xxx/xxx.aux"
    if [[ ! -e "$TARGET" ]]; then
        die "路径不存在：$TARGET"
    else
        die "$TARGET 不是 bundle 目录（里面没有 scene.json）"
    fi
elif [[ "$TARGET" == *.aux ]]; then
    [[ -f "$TARGET" ]] || die "找不到 $TARGET"
    AUX="$(cd "$(dirname "$TARGET")" && pwd)/$(basename "$TARGET")"
    BUNDLE="$(record_and_resolve "$AUX" "$(basename "$(dirname "$(dirname "$AUX")")")")"
else
    # 按 "设计名" 或 "套件/设计名" 查找
    name="${TARGET##*/}"
    want_suite=""
    [[ "$TARGET" == */* ]] && want_suite="${TARGET%/*}"
    mapfile -t hits < <(
        for d in "$ROOT"/benchmarks/*/"$name" "$ROOT/test_data/$name"; do
            [[ -f "$d/$name.aux" ]] || continue
            suite="$(basename "$(dirname "$d")")"
            [[ -z "$want_suite" || "$suite" == "$want_suite" ]] && printf '%s\t%s\n' "$suite" "$d/$name.aux"
        done)
    if [[ ${#hits[@]} -eq 0 ]]; then
        die "找不到设计 $TARGET（在 benchmarks/*/ 与 test_data/ 下都没有 $name/$name.aux）"
    elif [[ ${#hits[@]} -gt 1 ]]; then
        printf '\033[31m[viz]\033[0m 设计名 %s 有多份，请用 套件/设计 指定：\n' "$name" >&2
        for h in "${hits[@]}"; do printf '         scripts/viz.sh %s/%s\n' "${h%%$'\t'*}" "$name" >&2; done
        exit 1
    fi
    BUNDLE="$(record_and_resolve "${hits[0]#*$'\t'}" "${hits[0]%%$'\t'*}")"
fi

# ---- WSL2 下打开显卡加速；原生 Linux 有真正的 GPU 驱动，设了反而会出错
if [[ -z "${GALLIUM_DRIVER:-}" ]] && grep -qi microsoft /proc/version 2>/dev/null; then
    export GALLIUM_DRIVER=d3d12
fi

say "打开 $BUNDLE"
# 在 gui/ 下运行：查看器把面板布局记在当前目录的 superplace-gui.ini 里
cd "$ROOT/gui"
exec ./build/superplace-viz "$BUNDLE" ${PASS[@]+"${PASS[@]}"}
