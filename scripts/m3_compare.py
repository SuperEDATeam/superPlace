#!/usr/bin/env python3
"""Compare complete M3 audits; reject missing runs and invariant regressions."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import subprocess


def read_run(manifest):
    run = manifest.parent
    meta = json.loads(manifest.read_text())
    design = Path(meta['aux']).stem
    if not (run / 'completed.json').is_file():
        raise ValueError(f'incomplete: {run}')
    summary = json.loads((run / design / 'summary.json').read_text())
    log = (run / 'run.log').read_text()
    for key, value in summary.items():
        if isinstance(value, (float, int)) and not math.isfinite(value):
            raise ValueError(f'{run}: non-finite {key}')
    return run, meta, design, summary, log


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('baseline', type=Path)
    p.add_argument('candidate', type=Path)
    p.add_argument('--identical', action='store_true', help='require bit-identical init/gp PL')
    p.add_argument('--overlap', action='store_true', help='independent geometry check')
    p.add_argument('--expected-count', type=int, default=32, help='required seed-1002 designs')
    p.add_argument('--suite-aggregation', choices=['paired-designs', 'default-seed'],
                   default='paired-designs',
                   help='macro designs require seeds 1002,1,2,3,4 before equal-design aggregation')
    a = p.parse_args()
    failures, ratios, per_design, design_info = [], {}, {}, {}
    manifests = sorted(a.baseline.glob('*/*/seed-*/manifest.json'))
    if not manifests:
        p.error('no baseline runs')
    primary = sum(m.parent.name == 'seed-1002' for m in manifests)
    if primary != a.expected_count:
        failures.append(f'expected {a.expected_count} seed-1002 designs, found {primary}')
    for manifest in manifests:
        rel = manifest.relative_to(a.baseline)
        try:
            b, bm, design, bs, _ = read_run(manifest)
            n, nm, _, ns, log = read_run(a.candidate / rel)
            for key in ['target_density', 'target_overflow', 'cgp_target_overflow',
                        'random_seed', 'gp_bin_dim', 'density_chunks', 'poisson_backend']:
                if bm['config'].get(key) != nm['config'].get(key):
                    raise ValueError(f'incomparable config: {key}')
            if re.search(r'\[ERROR\]|\bNaN\b|\bInf\b|耗尽迭代上限', log, re.I):
                raise ValueError('ERROR/non-finite/iteration exhaustion in log')
            with (n / design / 'metrics.csv').open() as f:
                metrics = list(csv.DictReader(f))
            for row in metrics:
                for key, value in row.items():
                    if key != 'stage' and not math.isfinite(float(value)):
                        raise ValueError(f'non-finite CSV {row.get("stage")}/{row.get("iter")}: {key}')
            for key in ['mlg_overlap_after', 'cgp_macros_moved', 'gp_out_of_core', 'init_out_of_core']:
                if ns.get(key, 0) != 0:
                    raise ValueError(f'{key}={ns[key]}')
            if ns['init_hpwl'] != bs['init_hpwl']:
                raise ValueError('unchanged QP produced different HPWL')
            if nm['config'].get('lambda_schedule', 'fixed') == 'fixed':
                for key in ['mgp_hpwl', 'mgp_overflow', 'mgp_iterations']:
                    if key in bs and ns.get(key) != bs[key]:
                        raise ValueError(f'unchanged mGP changed: {key}')
                for key in ['mlg_macro_disp_mean', 'mlg_macro_disp_max']:
                    if key in bs and ns.get(key, math.inf) > bs[key] * (1.0 + 1e-6) + 1e-7:
                        raise ValueError(f'mLG displacement worsened: {key}')
            for stage in ['mgp', 'cgp']:
                key = stage + '_overflow'
                if key in bs and bs[key] < .1 and ns.get(key, math.inf) >= .1:
                    raise ValueError(f'new nonconvergence: {stage}')
            for stage in ['mGP', 'cGP']:
                key = stage.lower() + '_overflow'
                if ns.get(stage + '_converged', 0) and ns.get(key, math.inf) >= .1:
                    raise ValueError(f'{stage}: converged flag disagrees with delivered overflow')
                rollback = ns.get(stage + '_rollback_iter', -1)
                if rollback >= 0:
                    rows = [row for row in metrics
                            if row['stage'] == stage and int(row['iter']) == rollback]
                    if len(rows) != 1 or not math.isclose(float(rows[0]['overflow']), ns[key],
                                                        rel_tol=1e-6, abs_tol=1e-7):
                        raise ValueError(f'{stage}: rollback and delivered overflow disagree')
            for stage in ['init', 'gp']:
                left, right = b / design / f'{design}.{stage}.pl', n / design / f'{design}.{stage}.pl'
                if a.identical and hashlib.sha256(left.read_bytes()).digest() != hashlib.sha256(right.read_bytes()).digest():
                    raise ValueError(f'{stage}.pl changed')
            if a.overlap and ns.get('gp_movable_macros', 0):
                subprocess.run(['python3', 'scripts/check_overlap.py', str(Path(nm['aux']).parent),
                                str(n / design / f'{design}.gp.pl'), '--gate'], check=True,
                               stdout=subprocess.DEVNULL)
            suite = 'MMS' if rel.parts[0] == 'mms' else 'ISPD'
            ratio = ns['gp_hpwl'] / bs['gp_hpwl']
            seed = nm['config']['random_seed']
            design_key = rel.parts[:2]
            per_design.setdefault(design_key, {})[seed] = ratio
            design_info[design_key] = (suite, bs.get('gp_movable_macros', 0) > 0)
            print(f'{rel.parent}: HPWL {(ratio - 1) * 100:+.3f}%')
        except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as e:
            failures.append(f'{rel.parent}: {e}')
    fixed_seeds = [1002, 1, 2, 3, 4]
    for design, seeds in per_design.items():
        suite, has_macros = design_info[design]
        if 1002 not in seeds:
            failures.append(f'{design}: missing primary seed 1002')
            continue
        if a.suite_aggregation == 'paired-designs' and has_macros:
            missing = set(fixed_seeds) - seeds.keys()
            if missing:
                failures.append(f'{design}: five paired seeds incomplete: {sorted(missing)}')
                continue
            design_ratio = statistics.geometric_mean(seeds[s] for s in fixed_seeds)
            print(f'{design}: paired-five design HPWL {(design_ratio - 1) * 100:+.4f}%')
        else:
            design_ratio = seeds[1002]
        ratios.setdefault(suite, []).append(design_ratio)
    for suite, values in ratios.items():
        g = statistics.geometric_mean(values)
        print(f'{suite}: n={len(values)} HPWL geomean {(g - 1) * 100:+.4f}%')
        if g > 1.0 + 1e-12:
            failures.append(f'{suite}: geometric mean degraded')
    for design, seeds in per_design.items():
        required = set(fixed_seeds)
        if seeds.get(1002, 1.0) > 1.03:
            if not required.issubset(seeds):
                failures.append(f'{design}: HPWL >3%; five paired seeds required')
        if required.issubset(seeds) and statistics.geometric_mean(seeds[s] for s in required) > 1.03:
            failures.append(f'{design}: paired five-seed HPWL degraded >3%')
    for failure in failures:
        print('FAIL', failure)
    print(f'{len(manifests)} runs; {len(failures)} failures')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
