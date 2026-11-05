#!/usr/bin/env bash
# 重新生成黄金结果（test/golden/<设计>.sha256），供 ctest -L golden 比对。
#
# 只在【有意】改变计算结果时使用，并在提交说明里写明原因与影响面。
# 每个设计按 1/8/32 线程各跑一遍，三者哈希必须一致才写入——跨线程不一致是
# 铁律 7 的回归，不能被固化进黄金值；此时脚本报错退出，不改动任何文件。
#
# 用法：scripts/update_golden.sh [构建目录，默认 build]
#   可用环境变量 DESIGNS / THREADS 覆盖设计与线程数列表。
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=${1:-build}
CLI="$BUILD/superplace-cli"
CFG=configs/default.json
DESIGNS=${DESIGNS:-"thin1 adaptec1 mms_adaptec1"}
THREADS=${THREADS:-"1 8 32"}

[ -x "$CLI" ] || { echo "找不到 $CLI，先 cmake --build $BUILD" >&2; exit 1; }

for d in $DESIGNS; do
    aux=$(ls test_data/"$d"/*.aux 2>/dev/null | head -n1 || true)
    [ -n "$aux" ] || { echo "缺 test_data/$d（重新配置一次以解压 tar.xz）" >&2; exit 1; }
    bench=$(basename "$aux" .aux)

    ref=""
    for t in $THREADS; do
        out="$BUILD/golden/${d}_t$t"
        rm -rf "$out" && mkdir -p "$out"
        echo "== $d  $t 线程"
        "$CLI" --aux "$aux" --stage init,gp --config "$CFG" --threads "$t" --no-plot \
               --out "$out" > "$out/run.log" 2>&1 \
            || { echo "superplace-cli 失败，日志见 $out/run.log" >&2; exit 1; }
        sums=$(cd "$out/$bench" && sha256sum "$bench.init.pl" "$bench.gp.pl")
        echo "$sums"
        if [ -z "$ref" ]; then
            ref=$sums; ref_t=$t
        elif [ "$sums" != "$ref" ]; then
            echo "错误：$d 在 $t 线程与 $ref_t 线程下输出不一致，违反铁律 7，未写入黄金值" >&2
            exit 1
        fi
    done

    {
        echo "# $d：superplace-cli --stage init,gp --config $CFG --no-plot，线程数 $THREADS 下逐位一致"
        echo "# 由 scripts/update_golden.sh 生成，勿手工编辑"
        echo "$ref"
    } > "test/golden/$d.sha256"
    echo "已写入 test/golden/$d.sha256"
done
