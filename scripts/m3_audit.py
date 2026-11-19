#!/usr/bin/env python3
"""Reproducible M3 CPU audit. Outputs: experiment/suite/design/seed-N/."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

DENSITIES = dict.fromkeys(
    ['adaptec1', 'adaptec2', 'adaptec3', 'adaptec4',
     'bigblue1', 'bigblue2', 'bigblue3', 'bigblue4'], 1.0)
DENSITIES.update(dict.fromkeys(['adaptec5', 'newblue4', 'newblue5'], .5))
DENSITIES.update(dict.fromkeys(['newblue1', 'newblue3', 'newblue6', 'newblue7'], .8))
DENSITIES['newblue2'] = .9
DESIGNS = [f'{"ispd2005" if d.startswith("bigblue") or d in ["adaptec1", "adaptec2", "adaptec3", "adaptec4"] else "ispd2006"}/{d}' for d in DENSITIES]
DESIGNS += [f'mms/{d}' for d in DENSITIES]
PRIORITY = ['mms/newblue4', 'mms/newblue3', 'mms/bigblue2', 'mms/adaptec1']
DESIGNS = PRIORITY + [d for d in DESIGNS if d not in PRIORITY]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bin', required=True, type=Path)
    p.add_argument('--config', type=Path, default=Path('configs/default.json'))
    p.add_argument('--bench-root', type=Path, default=Path('/home/cai/EDA/GlobalPlacement/benchmarks'))
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--designs', nargs='+', default=DESIGNS)
    p.add_argument('--seeds', nargs='+', type=int, default=[1002])
    p.add_argument('--threads', type=int, default=16)
    p.add_argument('--calibration', type=Path, help='MMS adaptec1 diagnostic summary.json')
    p.add_argument('--relative-factor', type=float, choices=[.5, 1., 2.])
    a = p.parse_args()
    rejection = a.out / 'campaign-rejection.json'
    if rejection.is_file():
        raise SystemExit(f'Refusing rejected campaign: {rejection}')
    binary = a.bin.resolve()
    config = json.loads(a.config.read_text())
    if a.relative_factor is not None:
        if a.calibration is None:
            p.error('--relative-factor requires --calibration')
        calibration = json.loads(a.calibration.read_text())
        config['lambda_schedule'] = 'relative'
        config['delta_hpwl_relative'] = 350000.0 / calibration['mGP_initial_hpwl'] * a.relative_factor
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    diff = subprocess.check_output(['git', 'diff', 'HEAD', '--', '.', ':!configs/default.json'])
    failures = 0
    for item in a.designs:
        suite, design = item.split('/')
        aux = a.bench_root / suite / design / f'{design}.aux'
        if not aux.is_file():
            raise SystemExit(f'Missing official input: {aux}')
        for seed in a.seeds:
            run = a.out / suite / design / f'seed-{seed}'
            run.mkdir(parents=True, exist_ok=True)
            cfg = dict(config, target_density=DENSITIES[design], target_overflow=.1,
                       cgp_target_overflow=.1, poisson_backend='cpu', random_seed=seed,
                       num_threads=a.threads, fftw_threads=config.get('fftw_threads', 4),
                       plot=False, full_plot=False, frame_stride=0)
            provenance = dict(revision=revision, diff_sha256=hashlib.sha256(diff).hexdigest(),
                              binary_sha256=digest(binary), config=cfg,
                              aux=str(aux.resolve()), threads=a.threads,
                              machine=os.uname().nodename)
            source_manifest = binary.parent / 'source-manifest.json'
            if source_manifest.is_file():
                provenance['source_manifest'] = str(source_manifest)
                provenance['source_manifest_sha256'] = digest(source_manifest)
            manifest = run / 'manifest.json'
            done = run / 'completed.json'
            if done.exists():
                previous = json.loads(manifest.read_text())
                if any(previous[k] != provenance[k] for k in
                       ['revision', 'binary_sha256', 'config', 'aux', 'threads']):
                    raise SystemExit(f'Refusing incompatible resume: {run}')
                print(f'DONE {item} seed={seed}', flush=True)
                continue
            manifest.write_text(json.dumps(provenance, indent=2) + '\n')
            cfg_path = run / 'config.json'
            cfg_path.write_text(json.dumps(cfg, indent=2) + '\n')
            print(f'START {item} seed={seed}', flush=True)
            start = time.monotonic()
            with (run / 'run.log').open('w') as log:
                result = subprocess.run([str(binary), '--aux', str(aux), '--stage', 'init,gp',
                                         '--config', str(cfg_path), '--threads', str(a.threads),
                                         '--no-plot', '--out', str(run)], stdout=log,
                                        stderr=subprocess.STDOUT,
                                        env=dict(os.environ, OMP_WAIT_POLICY='PASSIVE'))
            status = dict(returncode=result.returncode, wall_s=time.monotonic() - start)
            summary = run / design / 'summary.json'
            valid = False
            if result.returncode == 0 and summary.is_file():
                try:
                    data = json.loads(summary.read_text())
                    valid = data.get('run_status') != 'failed'
                    if valid and data.get('fftw_threads', 1) != cfg['fftw_threads']:
                        valid = False
                        status['validation_error'] = 'CLI did not use requested fftw_threads'
                except (ValueError, OSError):
                    pass
            if valid:
                done.write_text(json.dumps(status, indent=2) + '\n')
            else:
                status.setdefault('validation_error', 'nonzero exit or missing/invalid/failed summary')
                failures += 1
            print(f'END {item} {status}', flush=True)
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
