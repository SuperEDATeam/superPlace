#!/usr/bin/env python3
"""从 density_batch 的布局日志提取 HPWL 与运行时间，写成一张 CSV。

日志名是 ``<suite>_<design>.log``。HPWL 取自 init 前、二次布局结束、mGP 结束、
gp 结束四行；时间取 parse / init / gp 三个 stage 的毫秒数，CSV 里换成秒。

用法：
    python3 scripts/summarize_density_logs.py
    python3 scripts/summarize_density_logs.py results/density_batch/logs results/density_batch/summary.csv
"""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUITES = ("ispd2005", "ispd2006", "mms")

# 规模从小到大，与 scripts/run_density_batch.sh 的顺序一致。
DESIGN_ORDER = [
    "adaptec1", "adaptec2", "bigblue1", "newblue1", "newblue2", "adaptec3",
    "newblue3", "adaptec4", "bigblue2", "newblue4", "adaptec5", "bigblue3",
    "newblue5", "newblue6", "bigblue4", "newblue7",
]

FIELDS = [
    "suite",
    "design",
    "hpwl_before",
    "hpwl_init",
    "hpwl_mgp",
    "hpwl_final",
    "parse_s",
    "init_s",
    "gp_s",
    "total_s",
]

NUM = r"([0-9.eE+-]+)"


def _last(pattern: str, text: str) -> re.Match[str] | None:
    found = None
    for found in re.finditer(pattern, text):
        pass
    return found


def _num(match: re.Match[str] | None, group: int = 1) -> str:
    if match is None:
        return ""
    return f"{float(match.group(group)):.0f}"


def _sec(match: re.Match[str] | None, group: int = 1) -> str:
    if match is None:
        return ""
    return f"{float(match.group(group)) / 1000.0:.3f}"


def parse_log(path: Path) -> dict[str, str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    suite, design = split_name(path.stem)
    before = _last(rf"init method:.*HPWL before:\s*{NUM}", text)
    init = _last(rf"init done: HPWL {NUM}\b", text)
    mgp = _last(rf"mGP 结束：.*HPWL={NUM}", text)
    final = _last(rf"gp done: HPWL {NUM} -> {NUM}", text)
    parse = _last(rf"parse done in {NUM} ms", text)
    init_t = _last(r"stage 'init' finished in " + NUM + r" ms", text)
    gp_t = _last(r"stage 'gp' finished in " + NUM + r" ms", text)

    parts = [parse, init_t, gp_t]
    if all(m is not None for m in parts):
        total = f"{sum(float(m.group(1)) for m in parts) / 1000.0:.3f}"
    else:
        total = ""

    return {
        "suite": suite,
        "design": design,
        "hpwl_before": _num(before),
        "hpwl_init": _num(init),
        "hpwl_mgp": _num(mgp),
        "hpwl_final": _num(final, 2) if final else "",
        "parse_s": _sec(parse),
        "init_s": _sec(init_t),
        "gp_s": _sec(gp_t),
        "total_s": total,
    }


def split_name(stem: str) -> tuple[str, str]:
    for suite in SUITES:
        prefix = suite + "_"
        if stem.startswith(prefix):
            return suite, stem[len(prefix):]
    raise ValueError(f"unrecognized log name: {stem}.log")


def sort_key(row: dict[str, str]) -> tuple[int, int, str]:
    try:
        design_i = DESIGN_ORDER.index(row["design"])
    except ValueError:
        design_i = len(DESIGN_ORDER)
    suite_i = SUITES.index(row["suite"]) if row["suite"] in SUITES else len(SUITES)
    return design_i, suite_i, row["design"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "log_dir",
        nargs="?",
        type=Path,
        default=ROOT / "results" / "density_batch" / "logs",
    )
    parser.add_argument(
        "out_csv",
        nargs="?",
        type=Path,
        default=ROOT / "results" / "density_batch" / "summary.csv",
    )
    args = parser.parse_args()

    logs = sorted(args.log_dir.glob("*.log"))
    if not logs:
        raise SystemExit(f"no logs in {args.log_dir}")

    rows = [parse_log(path) for path in logs]
    rows.sort(key=sort_key)

    args.out_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.out_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)

    missing = [r for r in rows if not r["hpwl_final"] or not r["total_s"]]
    print(f"wrote {len(rows)} rows to {args.out_csv}")
    if missing:
        names = ", ".join(f"{r['suite']}/{r['design']}" for r in missing)
        print(f"incomplete: {names}")


if __name__ == "__main__":
    main()
