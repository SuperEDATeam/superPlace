#!/usr/bin/env python3
"""把布局结果打包成前端可直接消费的 viz bundle（superplace-viz/1）。

这个脚本是**算法与前端之间唯一的连接点**。前端只认 bundle 这个文件格式，
不认识 C++、不链接 libsuperplace、不需要 FFTW/Eigen/libpng/OpenMP。
反过来算法侧也完全不知道前端的存在——本脚本是只读的，一行引擎代码都不改。

用法
----

从一次真实运行的产物打包（需要 benchmark 原始 .aux 与 results 目录）：

    python3 scripts/make_viz.py \\
        --aux benchmarks/ispd2006/newblue1/newblue1.aux \\
        --result results/audit/newblue1 \\
        --out gui/public/data/newblue1

造一个合成小样本（**不需要任何 benchmark 数据**，前端可立即开工）：

    python3 scripts/make_viz.py --synthetic --out gui/sample-data/synthetic

格式说明见 work_report/06-前端开发方案与接口契约.md §3。
"""

from __future__ import annotations

import argparse
import json
import math
import random
import re
import shutil
import struct
import sys
from pathlib import Path

FORMAT_VERSION = "superplace-viz/1"

# 与引擎 src/db/place_db.h 的 NodeFlag 保持一致。改这里必须同步改那边。
F_MACRO, F_FIXED, F_FILLER, F_NI = 1, 2, 4, 8

PL_RE = re.compile(r"^\s*(\S+)\s+(\S+)\s+(\S+)\s*:\s*(\S+)(.*)$")


# ---------------------------------------------------------------- BookShelf 读取
def read_aux(aux: Path) -> dict[str, Path]:
    text = aux.read_text()
    files = {}
    for tok in text.split():
        p = aux.parent / tok
        if p.suffix in (".nodes", ".nets", ".pl", ".scl", ".wts"):
            files[p.suffix[1:]] = p
    return files


def read_nodes(path: Path) -> tuple[list[str], list[tuple[float, float, int]]]:
    """解析 .nodes。

    不能按缩进判定节点行：ISPD2005 写作 `\\to0\\t8\\t12`（有前导制表符），
    ISPD2006 写作 `o0\\t12\\t12`（没有）。改为按"第 2、3 个 token 是不是数"判定，
    两种格式都覆盖，而 `NumNodes : 330474` 这类头部行因为第 2 个 token 是 `:`
    自然被排除。
    """
    names: list[str] = []
    info: list[tuple[float, float, int]] = []
    for line in path.read_text().splitlines():
        s = line.lstrip()
        if not s or s.startswith("#") or s.startswith("UCLA"):
            continue
        tok = line.split()
        if len(tok) < 3:
            continue
        try:
            w, h = float(tok[1]), float(tok[2])
        except ValueError:
            continue
        flags = 0
        if len(tok) >= 4 and tok[3].startswith("terminal"):
            flags |= F_FIXED
            if tok[3] == "terminal_NI":
                flags |= F_NI
        names.append(tok[0])
        info.append((w, h, flags))
    return names, info


def read_pl(path: Path) -> dict[str, tuple[float, float]]:
    pos = {}
    for line in path.read_text().splitlines():
        if line.lstrip().startswith("#") or line.startswith("UCLA"):
            continue
        m = PL_RE.match(line)
        if not m:
            continue
        try:
            pos[m.group(1)] = (float(m.group(2)), float(m.group(3)))
        except ValueError:
            continue
    return pos


def read_scl(path: Path) -> tuple[list[dict], float]:
    rows, heights = [], []

    def field(block: str, key: str) -> float:
        m = re.search(rf"{key}\s*:\s*(\S+)", block)
        return float(m.group(1)) if m else 0.0

    for m in re.finditer(r"CoreRow\s+Horizontal(.*?)\bEnd\b", path.read_text(), re.S):
        b = m.group(1)
        h = field(b, "Height")
        rows.append({
            "ly": field(b, "Coordinate"),
            "height": h,
            "lx": field(b, "SubrowOrigin"),
            "width": field(b, "NumSites") * max(field(b, "Sitewidth"), 1.0),
        })
        heights.append(h)
    row_h = max(set(heights), key=heights.count) if heights else 0.0
    return rows, row_h


# ------------------------------------------------------------------- bundle 写出
class Bundle:
    """按 superplace-viz/1 组织输出目录。

    几何与位置分开存：几何（w/h/flags）整场不变，前端下载一次就够；
    位置每帧都变，是动画的全部载荷。混在一起会让每帧多传一倍数据。
    """

    def __init__(self, out: Path, benchmark: str):
        self.out = out
        self.out.mkdir(parents=True, exist_ok=True)
        (self.out / "frames").mkdir(exist_ok=True)
        self.scene: dict = {
            "format": FORMAT_VERSION,
            "benchmark": benchmark,
            "flagBits": {"macro": F_MACRO, "fixed": F_FIXED, "filler": F_FILLER, "ni": F_NI},
            "frames": [],
        }

    def write_geometry(self, sizes: list[tuple[float, float]], flags: list[int]) -> None:
        n = len(sizes)
        buf = bytearray()
        for w, h in sizes:
            buf += struct.pack("<ff", w, h)
        (self.out / "geom.bin").write_bytes(bytes(buf))
        (self.out / "flags.bin").write_bytes(bytes(bytearray(flags)))
        self.scene["nodeCount"] = n

    def add_frame(self, idx: int, xs: list[float], ys: list[float], meta: dict) -> None:
        """位置按【中心坐标】存。

        BookShelf 的 .pl 用左下角，引擎内部用中心（铁律 2）。bundle 统一用中心，
        前端画矩形时自己减半宽半高——只在一处做换算，不给前端留第二套约定。
        """
        name = f"frames/f{idx:04d}.bin"
        buf = bytearray()
        buf += struct.pack(f"<{len(xs)}f", *xs)
        buf += struct.pack(f"<{len(ys)}f", *ys)
        (self.out / name).write_bytes(bytes(buf))
        self.scene["frames"].append({"file": name, **meta})

    def finish(self, **extra) -> None:
        self.scene.update(extra)
        (self.out / "scene.json").write_text(
            json.dumps(self.scene, indent=2, ensure_ascii=False) + "\n")


# ------------------------------------------------------------------ 真实数据转换
def convert(aux: Path, result: Path, out: Path, max_frames: int) -> int:
    files = read_aux(aux)
    for key in ("nodes", "pl", "scl"):
        if key not in files:
            print(f"错误：{aux} 中缺少 .{key}", file=sys.stderr)
            return 1

    names, info = read_nodes(files["nodes"])
    rows, row_h = read_scl(files["scl"])
    design = aux.stem

    # 候选帧：原始 .pl 作为第 0 帧，再按阶段顺序取 results 下的各阶段快照
    candidates: list[tuple[str, Path]] = [("input", files["pl"])]
    for stage in ("parse", "init", "gp"):
        p = result / f"{design}.{stage}.pl"
        if p.is_file():
            candidates.append((stage, p))
    snaps = sorted((result / "frames").glob("*.pl")) if (result / "frames").is_dir() else []
    candidates += [(f"gp", p) for p in snaps]

    if max_frames > 0 and len(candidates) > max_frames:
        step = len(candidates) / max_frames
        keep = sorted({int(i * step) for i in range(max_frames)} | {len(candidates) - 1})
        candidates = [candidates[i] for i in keep]

    # 以第一帧确定节点顺序；后续帧必须用同一顺序，否则前端的下标对不上
    bundle = Bundle(out, design)
    sizes = [(w, h) for w, h, _ in info]
    flags = [f for _, _, f in info]

    # 宏的判定与引擎一致：非固定且高度 > 1.5 倍行高（src/db/place_db.h）
    for i, (w, h, f) in enumerate(info):
        if not (f & F_FIXED) and row_h > 0 and h > row_h * 1.5:
            flags[i] |= F_MACRO
    bundle.write_geometry(sizes, flags)

    core = {
        "lx": min(r["lx"] for r in rows),
        "ly": min(r["ly"] for r in rows),
        "hx": max(r["lx"] + r["width"] for r in rows),
        "hy": max(r["ly"] + r["height"] for r in rows),
    }

    chip = dict(core)
    for idx, (stage, pl) in enumerate(candidates):
        pos = read_pl(pl)
        xs, ys = [], []
        for name, (w, h, _) in zip(names, info):
            x, y = pos.get(name, (0.0, 0.0))
            xs.append(x + w / 2.0)   # 左下角 -> 中心
            ys.append(y + h / 2.0)
            chip["lx"] = min(chip["lx"], x)
            chip["ly"] = min(chip["ly"], y)
            chip["hx"] = max(chip["hx"], x + w)
            chip["hy"] = max(chip["hy"], y + h)
        bundle.add_frame(idx, xs, ys, {"stage": stage, "label": f"{stage} ({pl.name})"})
        print(f"  帧 {idx}: {stage:6s} <- {pl}")

    metrics = result / "metrics.csv"
    if metrics.is_file():
        shutil.copy2(metrics, out / "metrics.csv")
    summary = result / "summary.json"
    if summary.is_file():
        shutil.copy2(summary, out / "summary.json")

    bundle.finish(core=core, chip=chip, rowHeight=row_h, rows=len(rows),
                  hasMetrics=metrics.is_file(), hasSummary=summary.is_file())
    print(f"{design}: {len(names)} 个节点，{len(bundle.scene['frames'])} 帧 -> {out}")
    return 0


# --------------------------------------------------------------------- 合成样本
def synthetic(out: Path, n_cells: int, n_macros: int, n_frames: int, seed: int) -> int:
    """造一个结构逼真但很小的 bundle，让前端在零数据依赖下就能开工。

    刻意覆盖前端必须处理的全部形态：标准单元、可移动宏、core 外的固定 I/O pad、
    零面积 terminal_NI、filler，以及"从中心一团逐步铺开"的收敛过程。
    """
    rng = random.Random(seed)
    core = {"lx": 0.0, "ly": 0.0, "hx": 4000.0, "hy": 4000.0}
    row_h = 12.0

    sizes: list[tuple[float, float]] = []
    flags: list[int] = []
    # 1) 标准单元
    for _ in range(n_cells):
        sizes.append((rng.choice([8, 12, 16, 24, 32]), row_h))
        flags.append(0)
    # 2) 可移动宏
    for _ in range(n_macros):
        sizes.append((rng.uniform(150, 420), rng.uniform(120, 480)))
        flags.append(F_MACRO)
    n_movable = len(sizes)
    # 3) 固定 I/O pad，贴在 core 外一圈（真实数据就是这样，前端不能假设都在 core 内）
    n_pads = 48
    for _ in range(n_pads):
        sizes.append((40.0, 20.0))
        flags.append(F_FIXED)
    # 4) 零面积 terminal_NI，必须被前端跳过而不是画成退化矩形
    for _ in range(8):
        sizes.append((0.0, 0.0))
        flags.append(F_FIXED | F_NI)
    n_fixed_end = len(sizes)
    # 5) filler
    free = (core["hx"] - core["lx"]) * (core["hy"] - core["ly"])
    used = sum(w * h for w, h in sizes[:n_movable])
    fw, fh = 14.0, row_h
    n_filler = max(0, int((free * 0.9 - used) / (fw * fh)))
    n_filler = min(n_filler, n_cells)        # 合成样本不必铺满，控制体积
    for _ in range(n_filler):
        sizes.append((fw, fh))
        flags.append(F_FILLER)
    n_total = len(sizes)

    # 固定节点的位置整场不变：pad 沿 core 四边排开
    fixed_pos: dict[int, tuple[float, float]] = {}
    for k in range(n_pads):
        i = n_movable + k
        side, t = k % 4, (k // 4) / max(1, n_pads // 4 - 1)
        span = 3600.0 * t + 200.0
        fixed_pos[i] = {
            0: (span, -40.0), 1: (span, 4040.0),
            2: (-40.0, span), 3: (4040.0, span),
        }[side]
    for k in range(8):
        fixed_pos[n_movable + n_pads + k] = (rng.uniform(0, 4000), rng.uniform(0, 4000))

    bundle = Bundle(out, "synthetic")
    bundle.write_geometry(sizes, flags)

    cx0, cy0 = 2000.0, 2000.0
    # 每个节点一个最终目标位置；按 t 从"中心一团"插值到目标，模拟 ePlace 的铺开过程
    targets = []
    for i in range(n_total):
        w, h = sizes[i]
        targets.append((rng.uniform(core["lx"] + w / 2, core["hx"] - w / 2),
                        rng.uniform(core["ly"] + h / 2, core["hy"] - h / 2)))

    for f in range(n_frames):
        t = f / max(1, n_frames - 1)
        ease = t * t * (3 - 2 * t)          # smoothstep，像真实收敛那样先慢后快再稳
        xs, ys = [], []
        for i in range(n_total):
            if i in fixed_pos:
                w, h = sizes[i]
                xs.append(fixed_pos[i][0] + w / 2)
                ys.append(fixed_pos[i][1] + h / 2)
                continue
            jitter = (1 - ease) * 30.0
            xs.append(cx0 + (targets[i][0] - cx0) * ease + rng.uniform(-jitter, jitter))
            ys.append(cy0 + (targets[i][1] - cy0) * ease + rng.uniform(-jitter, jitter))
        stage = "mGP" if t < 0.75 else "cGP"
        bundle.add_frame(f, xs, ys, {"stage": stage, "label": f"{stage} iter {f * 10}"})

    # 配一份形状合理的 metrics.csv：HPWL 先升后降、τ 单调下降，与真实曲线同形
    lines = ["stage,iter,hpwl,overflow,lambda,gamma,step_size,"
             "grad_norm_wl,grad_norm_den,max_displacement,elapsed_ms"]
    for f in range(n_frames):
        t = f / max(1, n_frames - 1)
        hpwl = 1.2e6 * (1 + 2.2 * math.sin(min(t, 0.6) * math.pi / 1.2)) * (1 - 0.12 * t)
        tau = max(0.06, 0.95 * math.exp(-3.2 * t))
        stage = "mGP" if t < 0.75 else "cGP"
        lines.append(f"{stage},{f * 10},{hpwl:.4f},{tau:.6f},{1e-8 * (1 + 40 * t):.6g},"
                     f"{600 * math.exp(-2.5 * t):.4f},{50 * math.exp(-2 * t):.4f},"
                     f"{1e5 * (1 - 0.5 * t):.4f},{8e4 * (1 - 0.3 * t):.4f},0,{f * 42.0:.1f}")
    (out / "metrics.csv").write_text("\n".join(lines) + "\n")

    chip = {"lx": -100.0, "ly": -100.0, "hx": 4100.0, "hy": 4100.0}
    bundle.finish(core=core, chip=chip, rowHeight=row_h,
                  rows=int((core["hy"] - core["ly"]) / row_h),
                  hasMetrics=True, hasSummary=False,
                  segments={"movable": [0, n_movable],
                            "fixed": [n_movable, n_fixed_end],
                            "filler": [n_fixed_end, n_total]})
    total_mb = sum(p.stat().st_size for p in out.rglob("*")) / 1e6
    print(f"合成样本：{n_total} 个节点（单元 {n_cells} / 宏 {n_macros} / 固定 "
          f"{n_fixed_end - n_movable} / filler {n_filler}），{n_frames} 帧，"
          f"{total_mb:.2f} MB -> {out}")
    return 0


# --------------------------------------------------------------------- 契约校验
def verify(path: Path) -> int:
    """校验一个 bundle 是否符合 superplace-viz/1。

    格式争议应该由程序判定而不是靠人对文档。前后端任何一方改了格式，
    这个脚本就是仲裁者；它过不了，就不要提交。
    """
    errs: list[str] = []

    def need(cond: bool, msg: str) -> None:
        if not cond:
            errs.append(msg)

    scene_path = path / "scene.json"
    if not scene_path.is_file():
        print(f"✗ 缺少 {scene_path}", file=sys.stderr)
        return 1
    scene = json.loads(scene_path.read_text())

    need(scene.get("format") == FORMAT_VERSION,
         f"format 应为 {FORMAT_VERSION}，实得 {scene.get('format')!r}")
    n = scene.get("nodeCount")
    need(isinstance(n, int) and n > 0, f"nodeCount 非法：{n!r}")
    for key in ("core", "chip"):
        r = scene.get(key, {})
        need(all(k in r for k in ("lx", "ly", "hx", "hy")), f"{key} 缺少 lx/ly/hx/hy")
        if all(k in r for k in ("lx", "hx")):
            need(r["hx"] > r["lx"] and r["hy"] > r["ly"], f"{key} 退化（hx<=lx 或 hy<=ly）")

    if isinstance(n, int) and n > 0:
        geom = path / "geom.bin"
        flags = path / "flags.bin"
        need(geom.is_file(), "缺少 geom.bin")
        need(flags.is_file(), "缺少 flags.bin")
        if geom.is_file():
            need(geom.stat().st_size == n * 8,
                 f"geom.bin 应为 {n * 8} 字节（nodeCount×2×float32），实得 {geom.stat().st_size}")
        if flags.is_file():
            need(flags.stat().st_size == n,
                 f"flags.bin 应为 {n} 字节，实得 {flags.stat().st_size}")

        frames = scene.get("frames", [])
        need(len(frames) >= 1, "frames 为空，至少要有一帧")
        # 帧的错误按类聚合再报。nodeCount 填错会让每一帧都不合尺寸，
        # 逐帧打印只会刷屏，把真正的根因淹掉。
        missing, wrong = [], []
        for i, fr in enumerate(frames):
            fp = path / fr.get("file", "")
            if not fp.is_file():
                missing.append(fr.get("file"))
            elif fp.stat().st_size != n * 8:
                wrong.append((fr["file"], fp.stat().st_size))
        if missing:
            errs.append(f"{len(missing)} 个帧文件不存在，首个：{missing[0]!r}")
        if wrong:
            errs.append(f"{len(wrong)} 个帧文件尺寸不符（应为 {n * 8} 字节），"
                        f"首个：{wrong[0][0]} 实得 {wrong[0][1]}")

    if scene.get("hasMetrics") and not (path / "metrics.csv").is_file():
        errs.append("hasMetrics=true 但没有 metrics.csv")

    if errs:
        print(f"✗ {path} 不符合 {FORMAT_VERSION}：", file=sys.stderr)
        for e in errs:
            print(f"    - {e}", file=sys.stderr)
        return 1
    total = sum(p.stat().st_size for p in path.rglob("*") if p.is_file()) / 1e6
    print(f"✓ {path} 符合 {FORMAT_VERSION}："
          f"{n} 个节点，{len(scene.get('frames', []))} 帧，{total:.2f} MB")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="打包前端 viz bundle")
    ap.add_argument("--verify", type=Path, help="校验一个已有 bundle 是否符合格式，然后退出")
    ap.add_argument("--aux", type=Path, help="benchmark 的 .aux")
    ap.add_argument("--result", type=Path, help="results/<design> 目录")
    ap.add_argument("--out", type=Path, help="输出 bundle 目录")
    ap.add_argument("--max-frames", type=int, default=0, help="最多保留多少帧，0 = 全部")
    ap.add_argument("--synthetic", action="store_true", help="生成合成样本，不读任何 benchmark")
    ap.add_argument("--cells", type=int, default=4000)
    ap.add_argument("--macros", type=int, default=24)
    ap.add_argument("--frames", type=int, default=40)
    ap.add_argument("--seed", type=int, default=1002)
    args = ap.parse_args()

    if args.verify:
        return verify(args.verify)
    if not args.out:
        print("错误：需要 --out（或用 --verify 校验已有 bundle）", file=sys.stderr)
        return 1
    if args.synthetic:
        return synthetic(args.out, args.cells, args.macros, args.frames, args.seed)
    if not args.aux or not args.result:
        print("错误：真实数据模式需要同时给 --aux 与 --result", file=sys.stderr)
        return 1
    return convert(args.aux, args.result, args.out, args.max_frames)


if __name__ == "__main__":
    sys.exit(main())
