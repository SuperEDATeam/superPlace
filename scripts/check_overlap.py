#!/usr/bin/env python3
"""宏合法化结果的独立核查：从输出 .pl 重算宏-宏与宏-阻挡的重叠，并按类型分账。

与 mLG 内部的 OverlapModel 完全独立（不同语言、不同实现），用来交叉验证它报的残留重叠。
用法：scripts/check_overlap.py <design.aux 所在目录> <输出的 .gp.pl>
"""
import collections, re, sys
from pathlib import Path

d = Path(sys.argv[1]); pl = Path(sys.argv[2]); name = d.name
row = collections.Counter(float(m.group(1)) for m in
                          re.finditer(r"Height\s*:\s*(\S+)", (d / f"{name}.scl").read_text())).most_common(1)[0][0]
nodes = {}
for line in open(d / f"{name}.nodes"):
    t = line.split()
    if len(t) < 3 or line.lstrip().startswith("#") or not t[0][0].isalpha(): continue
    try: w, h = float(t[1]), float(t[2])
    except ValueError: continue
    nodes[t[0]] = (w, h, len(t) >= 4 and t[3].startswith("terminal"))
pos = {}
for line in open(pl):
    t = line.split()
    if len(t) >= 3 and t[0] in nodes:
        try: pos[t[0]] = (float(t[1]), float(t[2]))
        except ValueError: pass
macros = [n for n, (w, h, term) in nodes.items() if not term and h > row * 1.5]
blocks = [n for n, (w, h, term) in nodes.items() if term and w > 0 and h > 0]
R = lambda n: (pos[n][0], pos[n][1], pos[n][0] + nodes[n][0], pos[n][1] + nodes[n][1])
def ov(a, b):
    dx = min(a[2], b[2]) - max(a[0], b[0]); dy = min(a[3], b[3]) - max(a[1], b[1])
    return dx * dy if dx > 0 and dy > 0 else 0.0
cell = 400.0
keys = lambda r: ((gx, gy) for gx in range(int(r[0] // cell), int(r[2] // cell) + 1)
                  for gy in range(int(r[1] // cell), int(r[3] // cell) + 1))
gm, gb = collections.defaultdict(list), collections.defaultdict(list)
for i, n in enumerate(macros):
    for k in keys(R(n)): gm[k].append(i)
for j, n in enumerate(blocks):
    for k in keys(R(n)): gb[k].append(j)
mm = mb = 0.0; pm = pb = 0; worst = []
for i, n in enumerate(macros):
    r = R(n); sm = set(); sb = set()
    for k in keys(r):
        for j in gm[k]:
            if j > i and j not in sm:
                sm.add(j); o = ov(r, R(macros[j]))
                if o > 0: mm += o; pm += 1; worst.append((o, "宏-宏", n, macros[j]))
        for j in gb[k]:
            if j not in sb:
                sb.add(j); o = ov(r, R(blocks[j]))
                if o > 0: mb += o; pb += 1; worst.append((o, "宏-阻挡", n, blocks[j]))
print(f"宏 {len(macros)} 个，非零面积阻挡 {len(blocks)} 个")
print(f"宏-宏   重叠 {mm:.0f}（{pm} 对）")
print(f"宏-阻挡 重叠 {mb:.0f}（{pb} 对）")
for o, kind, a, b in sorted(worst, reverse=True)[:5]:
    print(f"  {kind:<5} {o:>10.0f}  {a} {nodes[a][0]:.0f}x{nodes[a][1]:.0f}  vs  {b} {nodes[b][0]:.0f}x{nodes[b][1]:.0f}")
