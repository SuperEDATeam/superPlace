#!/usr/bin/env python3
"""Paired seed audit; --validate proves PL identity against independent CLI runs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

from m3_audit import DENSITIES, digest

FOCUS = ['mms/newblue4', 'mms/newblue3', 'mms/bigblue2', 'mms/bigblue4',
         'ispd2006/newblue1', 'ispd2006/newblue2', 'ispd2005/bigblue3']
MACRO_DESIGNS = FOCUS + [f'mms/{d}' for d in DENSITIES if f'mms/{d}' not in FOCUS]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bin', type=Path, required=True)
    p.add_argument('--baseline', type=Path, default=Path('results/m3/baseline'))
    p.add_argument('--candidate', type=Path, default=Path('results/m3/final'))
    p.add_argument('--designs', nargs='+', default=MACRO_DESIGNS)
    p.add_argument('--seeds', nargs='+', type=int, default=[1, 2, 3, 4])
    p.add_argument('--validate', action='store_true')
    p.add_argument('--validation-out', type=Path, default=Path('results/m3/pair-validation'))
    a = p.parse_args()
    binary = a.bin.resolve()
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    cache = (binary.parent / 'CMakeCache.txt').read_text()
    reference = re.search(r'^M3_REFERENCE_COMMIT:STRING=(.+)$', cache, re.M).group(1)
    reference = subprocess.check_output(['git', 'rev-parse', reference], text=True).strip()
    diff = subprocess.check_output(['git', 'diff', 'HEAD', '--', '.', ':!configs/default.json'])
    for item in a.designs:
        suite, design = item.split('/')
        original = [a.baseline / item, a.candidate / item]
        calibration = original[0] / 'seed-1002'
        source = json.loads((calibration / 'manifest.json').read_text())
        base_config = json.loads((calibration / 'config.json').read_text())
        for seed in a.seeds:
            rel = Path(item) / f'seed-{seed}'
            targets = ([a.validation_out / 'baseline' / rel, a.validation_out / 'candidate' / rel]
                       if a.validate else [a.baseline / rel, a.candidate / rel])
            manifests = []
            for out, policy in zip(targets, ['reference', 'current']):
                out.mkdir(parents=True, exist_ok=True)
                cfg = dict(base_config, target_density=DENSITIES[design], target_overflow=.1,
                           cgp_target_overflow=.1, random_seed=seed, num_threads=16,
                           poisson_backend='cpu', plot=False, full_plot=False, frame_stride=0)
                meta = dict(revision=revision, diff_sha256=hashlib.sha256(diff).hexdigest(),
                    binary_sha256=digest(binary), config=cfg, aux=source['aux'], threads=16,
                    machine=os.uname().nodename, runner='superplace-m3-pair',
                    reference_commit=reference, policy=policy, shared_preprocessing='QP,mGP')
                manifests.append(meta)
                if (out / 'completed.json').exists():
                    previous = json.loads((out / 'manifest.json').read_text())
                    if any(previous.get(k) != meta[k] for k in
                           ['binary_sha256', 'config', 'aux', 'reference_commit', 'policy']):
                        raise SystemExit(f'Refusing incompatible paired resume: {out}')
            if all((out / 'completed.json').exists() for out in targets):
                print(f'DONE pair {item} seed={seed}', flush=True)
                continue
            for out, meta in zip(targets, manifests):
                (out / 'manifest.json').write_text(json.dumps(meta, indent=2) + '\n')
                (out / 'config.json').write_text(json.dumps(meta['config'], indent=2) + '\n')
            print(f'START pair {item} seed={seed}', flush=True)
            start = time.monotonic()
            subprocess.run([str(binary), source['aux'], str(targets[0] / 'config.json'),
                            str(targets[0]), str(targets[1])], check=True,
                           env=dict(os.environ, OMP_WAIT_POLICY='PASSIVE'))
            if a.validate:
                for out, expected in zip(targets, original):
                    expected = expected / f'seed-{seed}' / design
                    for stage in ['init', 'gp']:
                        name = f'{design}.{stage}.pl'
                        if digest(out / design / name) != digest(expected / name):
                            raise SystemExit(f'Paired identity check failed: {out}/{name}')
                print(f'IDENTICAL reference/current vs independent CLIs: {item} seed={seed}', flush=True)
            elapsed = time.monotonic() - start
            for out in targets:
                if not (out / design / 'summary.json').exists():
                    raise SystemExit(f'Missing paired summary: {out}')
                (out / 'completed.json').write_text(json.dumps(dict(returncode=0, wall_s=elapsed,
                    timing_note='shared QP/mGP and both tails; not an independent wall time')) + '\n')
            print(f'END pair {item} seed={seed} wall={elapsed:.1f}s', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
