#!/usr/bin/env python3
"""从 ISPD2005/2006 原版重建 MMS（Modern Mixed-Size）风格 benchmark。

背景：ISPD2005/2006 把**全部宏单元标记为固定**，因此混合尺寸布局的代码路径
（宏密度缩放、mLG 宏合法化、mGP/cGP 切换）在原版数据上一次都不会被执行。
MMS（Yan/Viswanathan/Chu, DAC 2009）正是为解决这一点而构造的，规则只有两条：

    1. 把宏单元从固定解放为可移动：
           .nodes:  o496037  726 1068  terminal        ->  o496037  726 1068
           .pl:     o496037  4005 4114 : N /FIXED      ->  o496037  4005 4114 : N
    2. 把真正的 I/O pad 尺寸置零（仍保持固定）。

用法：
    scripts/make_mms.py test_data/adaptec1 benchmarks/mms/adaptec1

--------------------------------------------------------------------------
两处本脚本必须自行决定、论文未给出的细节
--------------------------------------------------------------------------

**其一：宏单元与 I/O pad 的判别。** 论文只说"I/O pad 位于 core region 边界外
或紧贴边界"，未给出脚本。本脚本按**中心是否落在 core region 内**判别：

    中心在 core 内   -> 宏单元，解放为可移动
    中心在 core 外   -> I/O pad，尺寸置零、保持固定

在 adaptec1 上这条规则与尺寸特征**完全吻合**：480 个中心在 core 外的 terminal
恰好只有 432x72 与 72x432 两种尺寸（典型的四边 pad 环），其余 63 个在 core 内
的才是真宏（164x2136 / 500x2136 / 1206x2856 ...）。两个独立判据给出同一划分，
说明规则站得住。

而 adaptec4 的 1329 个 terminal **全部**中心在 core 内、最小尺寸也有 504x216
（18 个 row 高），即该设计的 .nodes 里根本没有独立的 I/O pad 对象。此时脚本
如实输出"0 个 pad、1329 个可移动宏"，这是规则的正确结果而非 bug。

**其二：尺寸置零时必须重新对中。** BookShelf 的引脚偏移是**相对对象中心**的
（已核对：adaptec1 的 o0 尺寸 8x12，其引脚偏移 -3/-5 落在 ±4 x ±6 内）。
若把一个 432x72 的 pad 就地改成 0x0，它的中心会从 (x+216, y+36) 跳回 (x, y)，
所有挂在该 pad 上的引脚随之平移几百单位，HPWL 被无谓地扰动。
因此置零的同时把坐标改写为原中心，保证引脚几何逐点不变。

I/O pad 写成 `terminal_NI` 而非 `terminal`：这是 BookShelf 中"零面积固定引脚"
的标准写法，我们的解析器据此置 F_FIXED|F_NI，在密度、whitespace、绘图三处
跳过它，正是零面积对象应有的语义。

--------------------------------------------------------------------------

重建结果与 IEEE DataPort 上的官方重建版未必逐字节一致（判别脚本不同），
因此**不能用于跨论文的 HPWL 横向比较**；但用于验证我们自己的混合尺寸代码
路径是否被正确执行是完全够的，这也是本脚本的唯一目的。
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

# .pl 行：  name  x  y  : ORIENT [/FIXED]
PL_RE = re.compile(r"^(\s*)(\S+)\s+(\S+)\s+(\S+)\s*:\s*(\S+)(.*)$")


def read_core(scl: Path) -> tuple[float, float, float, float, float]:
    """从 .scl 读出 core region 包围盒与行高（取众数）。"""
    text = scl.read_text()
    xs: list[float] = []
    ys: list[float] = []
    heights: list[float] = []

    def field(block: str, key: str) -> float:
        m = re.search(rf"{key}\s*:\s*(\S+)", block)
        if m is None:
            raise ValueError(f"{scl}: CoreRow 缺少字段 {key}")
        return float(m.group(1))

    for m in re.finditer(r"CoreRow\s+Horizontal(.*?)\bEnd\b", text, re.S):
        block = m.group(1)
        y = field(block, "Coordinate")
        h = field(block, "Height")
        origin = field(block, "SubrowOrigin")
        sites = field(block, "NumSites")
        width = field(block, "Sitewidth")
        xs += [origin, origin + sites * width]
        ys += [y, y + h]
        heights.append(h)

    if not xs:
        raise ValueError(f"{scl}: 未解析到任何 CoreRow")
    row_height = max(set(heights), key=heights.count)
    return min(xs), max(xs), min(ys), max(ys), row_height


def read_positions(pl: Path) -> dict[str, tuple[float, float]]:
    pos: dict[str, tuple[float, float]] = {}
    for line in pl.read_text().splitlines():
        m = PL_RE.match(line)
        if m and not line.lstrip().startswith("#"):
            pos[m.group(2)] = (float(m.group(3)), float(m.group(4)))
    return pos


def classify(src: Path, name: str) -> tuple[dict[str, tuple[float, float]], set[str], float]:
    """返回 (pad -> 新坐标（原中心）, 宏单元名集合, 行高)。"""
    lx, hx, ly, hy, row_height = read_core(src / f"{name}.scl")
    pos = read_positions(src / f"{name}.pl")

    pads: dict[str, tuple[float, float]] = {}
    macros: set[str] = set()
    for line in (src / f"{name}.nodes").read_text().splitlines():
        tok = line.split()
        if len(tok) < 4 or tok[3] != "terminal":
            continue  # 标准单元，或已是 terminal_NI（零面积，保持原样）
        node, w, h = tok[0], float(tok[1]), float(tok[2])
        x, y = pos[node]
        cx, cy = x + w / 2.0, y + h / 2.0
        if lx <= cx <= hx and ly <= cy <= hy:
            macros.add(node)
        else:
            pads[node] = (cx, cy)  # 置零后左下角即中心，引脚几何不变
    return pads, macros, row_height


def write_nodes(src: Path, dst: Path, name: str, pads: dict, macros: set) -> None:
    out: list[str] = []
    fixed_after = 0
    num_terminals_at = -1
    for line in (src / f"{name}.nodes").read_text().splitlines():
        if line.lstrip().startswith("#"):
            out.append(line)
            continue
        if line.startswith("NumTerminals"):
            num_terminals_at = len(out)
            out.append(line)  # 占位，计数完成后回填
            continue
        tok = line.split()
        if len(tok) >= 4 and tok[0] in macros:
            out.append(f"\t{tok[0]}\t{tok[1]}\t{tok[2]}")  # 解放为可移动
        elif len(tok) >= 4 and tok[0] in pads:
            out.append(f"\t{tok[0]}\t0\t0\tterminal_NI")  # 尺寸置零
            fixed_after += 1
        else:
            if len(tok) >= 4 and tok[3].startswith("terminal"):
                fixed_after += 1  # 原本就是 terminal_NI，保持不动
            out.append(line)

    if num_terminals_at < 0:
        raise ValueError(f"{name}.nodes 中未找到 NumTerminals 行")
    out[num_terminals_at] = f"NumTerminals : \t\t{fixed_after}"
    (dst / f"{name}.nodes").write_text("\n".join(out) + "\n")


def write_pl(src: Path, dst: Path, name: str, pads: dict, macros: set) -> None:
    out: list[str] = []
    for line in (src / f"{name}.pl").read_text().splitlines():
        m = PL_RE.match(line)
        if m is None or line.lstrip().startswith("#"):
            out.append(line)
            continue
        indent, node, x, y, orient, tail = m.groups()
        if node in macros:
            out.append(f"{indent}{node}\t{x}\t{y}\t: {orient}")  # 去掉 /FIXED
        elif node in pads:
            nx, ny = pads[node]
            out.append(f"{indent}{node}\t{nx:g}\t{ny:g}\t: {orient}{tail}")
        else:
            out.append(line)
    (dst / f"{name}.pl").write_text("\n".join(out) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description="从 ISPD2005/2006 重建 MMS 风格 benchmark")
    ap.add_argument("src", type=Path, help="源设计目录（含 <name>.aux）")
    ap.add_argument("dst", type=Path, help="输出目录")
    args = ap.parse_args()

    src: Path = args.src
    aux = next(src.glob("*.aux"), None)
    if aux is None:
        print(f"错误：{src} 下没有 .aux 文件", file=sys.stderr)
        return 1
    name = aux.stem

    dst: Path = args.dst
    dst.mkdir(parents=True, exist_ok=True)

    pads, macros, row_height = classify(src, name)
    write_nodes(src, dst, name, pads, macros)
    write_pl(src, dst, name, pads, macros)
    for ext in ("aux", "nets", "wts", "scl"):
        f = src / f"{name}.{ext}"
        if f.exists():
            shutil.copy2(f, dst / f"{name}.{ext}")

    print(f"{name}: 解放宏单元 {len(macros)} 个，I/O pad 置零 {len(pads)} 个"
          f"（行高 {row_height:g}）")
    print(f"  -> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
