#!/usr/bin/env python3
"""把全局布局的逐迭代 PNG 合成收敛动画。

收敛动画不只是答辩素材，它是最高效的调试手段：塌缩到中心、炸到边界、
卡在 bin 边界形成条纹——这几类失效在指标曲线上都可能看着"还行"，
在动画里却一眼就能看出来。

先跑出帧：

    ./build/superplace-cli --aux <design>.aux --stage init,gp \\
        --out results --full-plot

再合成：

    python3 scripts/make_gif.py results/<design>

默认对帧做等间隔抽样，把动画控制在 `--max-frames` 张以内——
一次 mGP 有三百多轮，全画进去既大又快得看不清。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("需要 Pillow：pip install Pillow", file=sys.stderr)
    raise SystemExit(1)


def pick(frames: list[Path], limit: int) -> list[Path]:
    """等间隔抽样，但始终保留最后一帧——收敛终态是最该看的一张。"""
    if limit <= 0 or len(frames) <= limit:
        return frames
    step = len(frames) / limit
    idx = sorted({int(i * step) for i in range(limit)} | {len(frames) - 1})
    return [frames[i] for i in idx]


def main() -> int:
    ap = argparse.ArgumentParser(description="合成全局布局收敛动画")
    ap.add_argument("result_dir", type=Path, help="results/<design> 目录")
    ap.add_argument("--out", type=Path, default=None, help="输出 GIF 路径")
    ap.add_argument("--max-frames", type=int, default=60, help="最多保留多少帧（默认 60）")
    ap.add_argument("--fps", type=float, default=8.0, help="帧率（默认 8）")
    ap.add_argument("--width", type=int, default=600, help="缩放宽度，0 表示不缩放")
    ap.add_argument("--hold", type=float, default=1.5, help="末帧停留秒数（默认 1.5）")
    args = ap.parse_args()

    plots: Path = args.result_dir / "plots"
    if not plots.is_dir():
        print(f"错误：{plots} 不存在。是否忘了加 --full-plot？", file=sys.stderr)
        return 1

    frames = sorted(plots.glob("iter_*.png"))
    if not frames:
        print(f"错误：{plots} 下没有 iter_*.png。是否忘了加 --full-plot？", file=sys.stderr)
        return 1

    chosen = pick(frames, args.max_frames)
    images = []
    for f in chosen:
        im = Image.open(f).convert("RGB")
        if args.width > 0 and im.width != args.width:
            h = round(im.height * args.width / im.width)
            im = im.resize((args.width, h), Image.LANCZOS)
        # GIF 只有 256 色，提前量化成统一调色板，避免逐帧各自量化导致闪烁
        images.append(im.quantize(colors=128, method=Image.MEDIANCUT))

    out: Path = args.out or (args.result_dir / "plots" / "convergence.gif")
    per_frame_ms = max(20, int(1000.0 / max(args.fps, 0.1)))
    durations = [per_frame_ms] * len(images)
    durations[-1] = max(per_frame_ms, int(args.hold * 1000))

    images[0].save(out, save_all=True, append_images=images[1:], duration=durations, loop=0,
                   optimize=True, disposal=2)
    size_mb = out.stat().st_size / 1e6
    print(f"{len(frames)} 帧抽样为 {len(images)} 帧 -> {out} ({size_mb:.2f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
