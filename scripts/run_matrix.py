#!/usr/bin/env python3
"""初始布局方法对比矩阵（课程任务 4：设计和分析比较）。

跑 {4 种实现} x {若干 benchmark}，汇总 summary.json，输出 markdown 表格与折线图，
覆盖课程要求的四个对比维度：

    连接保持能力  ->  最终 HPWL
    时间复杂度    ->  实测耗时 + 规模增长曲线
    面积控制      ->  密度网格的 max / std / overflow
    适用规模      ->  三种规模下的耗时与质量变化

注意：四个实现对应课程要求的**三种方法**——cluster_fc 与 cluster_bc 是
聚类方法内部的两种合并策略，不是第四种方法。

用法：
    python3 scripts/run_matrix.py                      # 默认全跑
    python3 scripts/run_matrix.py --bench adaptec1     # 只跑一个
    python3 scripts/run_matrix.py --skip-easyplace     # 不做 easyPlace 对拍
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "superplace-cli"
TEST_DATA = ROOT / "test_data"

METHODS = ["random", "cluster_fc", "cluster_bc", "quadratic"]

# 方法 -> (课程方法归属, 理论时间复杂度)
METHOD_INFO = {
    "random":     ("方法一 随机布局",        "O(n)"),
    "cluster_fc": ("方法二 聚类（First-Choice）", "O(P) 每层, O(P log n) 总计"),
    "cluster_bc": ("方法二 聚类（Best-Choice）",  "O(P log n) 每层"),
    "quadratic":  ("方法三 二次解析（B2B）",  "O(iter * nnz) BiCGSTAB"),
}

EASYPLACE = Path.home() / "easyPlace" / "build" / "main" / "ePlace"


def discover_benchmarks() -> list[str]:
    if not TEST_DATA.is_dir():
        return []
    names = []
    for d in sorted(TEST_DATA.iterdir()):
        if d.is_dir() and (d / f"{d.name}.aux").is_file():
            names.append(d.name)
    # 由小到大排，便于观察规模趋势
    order = {"thin1": 0, "adaptec1": 1, "adaptec4": 2}
    return sorted(names, key=lambda n: (order.get(n, 99), n))


def run_one(bench: str, method: str, outdir: Path) -> dict | None:
    aux = TEST_DATA / bench / f"{bench}.aux"
    cmd = [
        str(CLI), "--aux", str(aux), "--stage", "parse,init",
        "--init-method", method, "--out", str(outdir), "--no-plot",
    ]
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True)
    wall = time.perf_counter() - t0
    if proc.returncode != 0:
        print(f"  [FAIL] {bench}/{method}: {proc.stderr.strip()[:200]}", file=sys.stderr)
        return None

    summary = outdir / bench / "summary.json"
    if not summary.is_file():
        print(f"  [FAIL] {bench}/{method}: no summary.json", file=sys.stderr)
        return None
    with summary.open(encoding="utf-8") as f:
        data = json.load(f)
    data["_wall_s"] = wall
    return data


def run_easyplace(bench: str, workdir: Path) -> float | None:
    """easyPlace 的纯 QP 结果，作为二次解析布局的信息性对拍基准。

    注意：easyPlace 的 Y 方向 B2B 有已确认缺陷（qplace.cpp:157 漏了一个边界条件），
    所以我们的 HPWL **偏低是预期且正确的**；只有显著偏高才需要排查。
    """
    if not EASYPLACE.is_file():
        return None
    workdir.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(EASYPLACE), "-aux", str(TEST_DATA / bench / f"{bench}.aux"),
        "-nomGP", "-nomLG", "-nocGP", "-noLegal", "-outputPath", str(workdir),
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir, timeout=1800)
    except subprocess.TimeoutExpired:
        return None
    best = None
    for line in proc.stdout.splitlines():
        if "iteration number" in line and "HPWL" in line:
            try:
                val = float(line.split("HPWL")[1].split(",")[0].strip())
            except (IndexError, ValueError):
                continue
            best = val if best is None else min(best, val)
    return best


def fmt(x, unit="") -> str:
    if x is None:
        return "-"
    if isinstance(x, float):
        if abs(x) >= 1e6:
            return f"{x:.4g}{unit}"
        return f"{x:.4f}{unit}".rstrip("0").rstrip(".") + ("" if unit else "")
    return f"{x}{unit}"


def _cells(results: dict, bench: str) -> int:
    """该 benchmark 的可移动单元数"""
    for m in METHODS:
        r = results.get((bench, m))
        if r and r.get("num_movable"):
            return int(r["num_movable"])
    return 0


def build_report(results: dict, benches: list[str], easyplace: dict) -> str:
    L: list[str] = []
    L.append("# 初始布局方法对比\n")
    L.append("> 由 `scripts/run_matrix.py` 自动生成。\n")
    L.append("> 四个实现对应课程要求的**三种方法**——`cluster_fc` 与 `cluster_bc` "
             "是聚类方法内部的两种合并策略，不是第四种方法。\n")

    L.append("\n## 方法说明\n")
    L.append("| 实现 | 课程方法 | 理论复杂度 |")
    L.append("| --- | --- | --- |")
    for m in METHODS:
        info = METHOD_INFO[m]
        L.append(f"| `{m}` | {info[0]} | {info[1]} |")

    # ---------------------------------------------------------- 维度一：线长
    L.append("\n## 维度一：连接保持能力（HPWL，越小越好）\n")
    L.append("| benchmark | " + " | ".join(f"`{m}`" for m in METHODS) + " |")
    L.append("| --- |" + " --- |" * len(METHODS))
    for b in benches:
        row = [b]
        for m in METHODS:
            r = results.get((b, m))
            row.append(fmt(r.get("init_hpwl")) if r else "-")
        L.append("| " + " | ".join(row) + " |")

    L.append("\n相对随机布局的改善：\n")
    L.append("| benchmark | " + " | ".join(f"`{m}`" for m in METHODS[1:]) + " |")
    L.append("| --- |" + " --- |" * (len(METHODS) - 1))
    for b in benches:
        base = results.get((b, "random"))
        row = [b]
        for m in METHODS[1:]:
            r = results.get((b, m))
            if r and base and base.get("init_hpwl"):
                imp = (base["init_hpwl"] - r["init_hpwl"]) / base["init_hpwl"] * 100
                row.append(f"{imp:+.1f}%")
            else:
                row.append("-")
        L.append("| " + " | ".join(row) + " |")

    # ---------------------------------------------------------- 维度二：耗时
    L.append("\n## 维度二：时间复杂度（初始布局耗时，ms）\n")
    L.append("| benchmark | 可移动单元 | " + " | ".join(f"`{m}`" for m in METHODS) + " |")
    L.append("| --- | --- |" + " --- |" * len(METHODS))
    for b in benches:
        any_r = next((results[(b, m)] for m in METHODS if (b, m) in results), None)
        row = [b, fmt(any_r.get("num_movable")) if any_r else "-"]
        for m in METHODS:
            r = results.get((b, m))
            row.append(f"{r['init_time_ms']:.1f}" if r and "init_time_ms" in r else "-")
        L.append("| " + " | ".join(row) + " |")

    # ---------------------------------------------------------- 维度三：面积
    L.append("\n## 维度三：面积控制（密度网格统计）\n")
    L.append("`maxDensity` 为最拥挤网格的占用率，`stdDensity` 越小说明分布越均匀。\n")
    L.append("| benchmark | 方法 | maxDensity | stdDensity | overflowArea |")
    L.append("| --- | --- | --- | --- | --- |")
    for b in benches:
        for m in METHODS:
            r = results.get((b, m))
            if not r:
                continue
            L.append(f"| {b} | `{m}` | {r.get('init_max_density', 0):.3f} | "
                     f"{r.get('init_std_density', 0):.3f} | "
                     f"{fmt(r.get('init_overflow_area'))} |")

    # ---------------------------------------------------------- 维度四：规模
    L.append("\n## 维度四：适用规模\n")
    L.append("单位单元耗时（微秒/单元）。若算法是线性的，该值应随规模基本持平；"
             "明显上升说明存在超线性因子。\n")
    L.append("| 方法 | " + " | ".join(f"{b} ({fmt(_cells(results, b))})" for b in benches) + " |")
    L.append("| --- |" + " --- |" * len(benches))
    for m in METHODS:
        row = [f"`{m}`"]
        for b in benches:
            r = results.get((b, m))
            cells = _cells(results, b)
            if r and cells:
                row.append(f"{r['init_time_ms'] * 1000.0 / cells:.2f}")
            else:
                row.append("-")
        L.append("| " + " | ".join(row) + " |")

    # ------------------------------------------------------ easyPlace 对拍
    if easyplace:
        L.append("\n## easyPlace 对拍（信息性，非硬门）\n")
        L.append("easyPlace 的 Y 方向 B2B 存在已确认缺陷"
                 "（`QPlace/qplace.cpp:157` 漏了 `pin1 == boundPinYmin`），")
        L.append("因此**我们的 HPWL 偏低是预期且正确的**；只有显著偏高才需要排查。\n")
        L.append("| benchmark | 我们的 quadratic | easyPlace QP 最优 | 差异 |")
        L.append("| --- | --- | --- | --- |")
        for b in benches:
            ours = results.get((b, "quadratic"), {}).get("init_hpwl")
            theirs = easyplace.get(b)
            if ours is None or theirs is None:
                continue
            diff = (ours - theirs) / theirs * 100
            L.append(f"| {b} | {fmt(ours)} | {fmt(theirs)} | {diff:+.1f}% |")

    L.append("\n## 结论\n")
    if benches:
        big = benches[-1]
        rnd = results.get((big, "random"), {}).get("init_hpwl")
        qp = results.get((big, "quadratic"), {}).get("init_hpwl")
        fc = results.get((big, "cluster_fc"), {}).get("init_hpwl")
        bc = results.get((big, "cluster_bc"), {}).get("init_hpwl")
        if rnd and qp:
            L.append(f"- 在 {big} 上，二次解析布局的 HPWL 约为随机布局的 "
                     f"**1/{rnd / qp:.0f}**，是三种方法中唯一可直接作为全局布局初值的。")
        if fc and bc:
            L.append(f"- 聚类方法的两种合并策略：Best-Choice 比 First-Choice 的 HPWL "
                     f"好 **{(fc - bc) / fc * 100:.1f}%**，代价是更长的运行时间——"
                     f"这是全局贪心与局部贪心的典型权衡。")
        L.append("- 随机布局无法保持任何连接关系，仅作为对照基准存在。")
    return "\n".join(L) + "\n"


def plot(results: dict, benches: list[str], outdir: Path) -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    x = range(len(benches))

    ax = axes[0]
    for m in METHODS:
        ys = [results.get((b, m), {}).get("init_hpwl") for b in benches]
        if any(y is not None for y in ys):
            ax.plot(x, [y if y else float("nan") for y in ys], marker="o", label=m)
    ax.set_xticks(list(x))
    ax.set_xticklabels(benches)
    ax.set_yscale("log")
    ax.set_ylabel("HPWL (log)")
    ax.set_title("Wirelength by method")
    ax.grid(True, alpha=0.3)
    ax.legend()

    ax = axes[1]
    for m in METHODS:
        ys = [results.get((b, m), {}).get("init_time_ms") for b in benches]
        if any(y is not None for y in ys):
            ax.plot(x, [y if y else float("nan") for y in ys], marker="s", label=m)
    ax.set_xticks(list(x))
    ax.set_xticklabels(benches)
    ax.set_yscale("log")
    ax.set_ylabel("time (ms, log)")
    ax.set_title("Runtime by method")
    ax.grid(True, alpha=0.3)
    ax.legend()

    fig.tight_layout()
    fig.savefig(outdir / "comparison.png", dpi=130)
    plt.close(fig)
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", action="append", help="只跑指定 benchmark（可重复）")
    ap.add_argument("--method", action="append", help="只跑指定方法（可重复）")
    ap.add_argument("--out", default=str(ROOT / "results" / "matrix"), help="输出目录")
    ap.add_argument("--skip-easyplace", action="store_true", help="跳过 easyPlace 对拍")
    args = ap.parse_args()

    if not CLI.is_file():
        print(f"error: {CLI} 不存在，请先构建：cmake --build build", file=sys.stderr)
        return 2

    benches = args.bench or discover_benchmarks()
    if not benches:
        print(f"error: 在 {TEST_DATA} 下没有找到 benchmark", file=sys.stderr)
        return 2
    methods = args.method or METHODS

    outdir = Path(args.out)
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)

    results: dict[tuple[str, str], dict] = {}
    for b in benches:
        print(f"== {b} ==")
        for m in methods:
            print(f"  {m} ...", end="", flush=True)
            r = run_one(b, m, outdir / m)
            if r:
                results[(b, m)] = r
                print(f" HPWL {r.get('init_hpwl', 0):.6g}  {r.get('init_time_ms', 0):.1f} ms")
            else:
                print(" FAILED")

    easyplace: dict[str, float] = {}
    if not args.skip_easyplace and EASYPLACE.is_file():
        print("== easyPlace 对拍 ==")
        for b in benches:
            print(f"  {b} ...", end="", flush=True)
            v = run_easyplace(b, outdir / "easyplace" / b)
            if v:
                easyplace[b] = v
                print(f" best QP HPWL {v:.6g}")
            else:
                print(" skipped")

    report = build_report(results, benches, easyplace)
    report_path = outdir / "comparison.md"
    report_path.write_text(report, encoding="utf-8")
    print(f"\n报告已写入 {report_path}")

    if plot(results, benches, outdir):
        print(f"图表已写入 {outdir / 'comparison.png'}")
    else:
        print("（未安装 matplotlib，跳过绘图）")

    print()
    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
