#!/usr/bin/env python3
"""对比两次审计：mGP 结束方式、mLG 残留重叠、最终 HPWL、耗时。
用法：scripts/compare_audit.py <基线目录> <新目录> [覆盖基线日志的 设计=路径 ...]"""
import csv, re, sys
from pathlib import Path

base, new = Path(sys.argv[1]), Path(sys.argv[2])
override = dict(a.split("=", 1) for a in sys.argv[3:])

def wall(d):
    f = d / "wall.csv"
    return {r["design"].split("/")[-1]: float(r["wall_s"]) for r in csv.DictReader(open(f))} if f.exists() else {}

def parse(p):
    t = Path(p).read_text() if Path(p).exists() else ""
    g = lambda pat: (re.findall(pat, t) or [None])[-1]
    m = re.search(r"mGP 结束：(\d+) 轮.*?τ=([0-9.]+)（([^）]+)）", t)
    ov = g(r"mLG: 重叠 \S+ -> (\S+)（")
    fin = g(r"cGP 结束：.*?HPWL=([0-9.e+]+)") or g(r"mGP 结束：.*?HPWL=([0-9.e+]+)")
    return dict(it=m and int(m.group(1)), tau=m and float(m.group(2)),
                how=m and m.group(3), ov=ov and float(ov), hpwl=fin and float(fin))

wb, wn = wall(base), wall(new)
names = [l.split("/")[-1] for l in (base / "order.txt").read_text().split()] if (base / "order.txt").exists() \
        else [r for r in wb]
short = {"达标": "收敛", "τ 停滞，已回退到最优解": "停滞", "耗尽迭代上限": "跑满"}
print(f"{'设计':10s} {'mGP 基线':>16s} {'mGP 新':>16s} {'残留重叠 基线→新':>22s} {'最终HPWL 基线':>13s} {'新':>11s} {'变化':>7s} {'耗时 基线→新':>14s}")
tb = tn = 0.0; hb = []
for d in names:
    b = parse(override.get(d, base / f"{d}.log")); n = parse(new / f"{d}.log")
    if not n["hpwl"]: continue
    fm = lambda r: f"{r['it']}轮 {short.get(r['how'], r['how'])} {r['tau']:.3f}" if r["it"] else "-"
    ch = (n["hpwl"] / b["hpwl"] - 1) * 100 if b["hpwl"] else float("nan")
    hb.append(ch)
    tb += wb.get(d, 0); tn += wn.get(d, 0)
    print(f"{d:10s} {fm(b):>16s} {fm(n):>16s} {b['ov'] or 0:>10.0f} → {n['ov'] or 0:<9.0f} {b['hpwl']:>13.4e} {n['hpwl']:>11.4e} {ch:>+6.2f}% {wb.get(d,0):>6.1f}→{wn.get(d,0):<6.1f}")
import statistics
print(f"\n共 {len(hb)} 个；HPWL 变化 几何均值 {(statistics.geometric_mean([1+x/100 for x in hb])-1)*100:+.2f}%，"
      f"最好 {min(hb):+.2f}%，最差 {max(hb):+.2f}%；总耗时 {tb/60:.1f} → {tn/60:.1f} min")
