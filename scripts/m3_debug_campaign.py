#!/usr/bin/env python3
"""Round-2 independent CLI regression: standard 108 runs plus four ISPD newblue4 seeds."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import threading

from m3_audit import DESIGNS, digest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bin', type=Path, required=True)
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--workers', type=int, choices=[1, 2], default=2)
    p.add_argument('--changed-stages', nargs='+', choices=['mgp', 'cgp'], default=[])
    p.add_argument('--identical', action='store_true')
    p.add_argument('--screen-ispd', action='store_true',
                   help='reject early if the complete ISPD suite fails; success still requires all 112 runs')
    a = p.parse_args()
    binary = a.bin.resolve()
    tasks = []
    for item in DESIGNS:
        seeds = [1002, 1, 2, 3, 4] if (item.startswith('mms/') or item in
            ['ispd2005/bigblue3','ispd2006/newblue1','ispd2006/newblue2','ispd2006/newblue4']) else [1002]
        for seed in seeds:
            reference = a.reference / item / f'seed-{seed}'
            if not (reference / 'completed.json').is_file():
                raise SystemExit(f'Missing complete reference: {reference}')
            status = json.loads((reference / 'completed.json').read_text())
            tasks.append((status.get('wall_s', 0), item, seed))
    assert len(tasks) == 112 and len({(i,s) for _,i,s in tasks}) == 112
    tasks.sort(reverse=True)  # Scheduling estimate only; never a performance comparison.
    a.out.mkdir(parents=True, exist_ok=True)
    rejection = a.out / 'campaign-rejection.json'
    if rejection.is_file():
        print(f'REJECTED campaign: {rejection}', flush=True)
        return 1
    (a.out / 'campaign.json').write_text(json.dumps(dict(
        binary=str(binary), binary_sha256=digest(binary), reference=str(a.reference.resolve()),
        changed_stages=a.changed_stages, identical=a.identical, workers=a.workers,
        screen_ispd=a.screen_ispd,
        threads_per_run=16, settings=[dict(design=i,seed=s) for _,i,s in tasks]),indent=2)+'\n')
    stop = threading.Event()
    def run(task):
        _, item, seed = task
        if stop.is_set() or rejection.is_file():
            stop.set()
            return False
        log = a.out / 'driver-logs' / item / f'seed-{seed}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open('w') as f:
            result = subprocess.run(['python3','-u','scripts/m3_audit.py','--bin',str(binary),
                '--out',str(a.out),'--designs',item,'--seeds',str(seed)],
                stdout=f,stderr=subprocess.STDOUT,env=dict(os.environ,OMP_WAIT_POLICY='PASSIVE'))
        if result.returncode:
            stop.set()
            print(f'FAILED {item} seed={seed}: {log}',flush=True)
            return False
        print(f'COMPLETE {item} seed={seed}',flush=True)
        return True
    phases = [[t for t in tasks if not t[1].startswith('mms/')],
              [t for t in tasks if t[1].startswith('mms/')]] if a.screen_ispd else [tasks]
    for index, phase in enumerate(phases):
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.workers) as pool:
            ok = all(list(pool.map(run,phase)))
        if not ok: return 1
        if a.screen_ispd and index == 0:
            assert len(phase) == 32
            reference = a.out / 'screen-reference'
            for _, item, seed in phase:
                link = reference / item / f'seed-{seed}'
                link.parent.mkdir(parents=True, exist_ok=True)
                if not link.exists():
                    link.symlink_to((a.reference/item/f'seed-{seed}').resolve(),target_is_directory=True)
            command = ['python3','-u','scripts/m3_compare.py',str(reference),str(a.out),
                       '--expected-count','16','--overlap']
            if a.changed_stages: command += ['--changed-stages',*a.changed_stages]
            if a.identical: command += ['--identical']
            with (a.out/'ispd-screen.txt').open('w') as f:
                result = subprocess.run(command,stdout=f,stderr=subprocess.STDOUT)
            if result.returncode:
                print('REJECTED complete ISPD suite; remaining MMS runs not executed',flush=True)
                return result.returncode
    command = ['python3','-u','scripts/m3_compare.py',str(a.reference),str(a.out),'--overlap']
    if a.changed_stages: command += ['--changed-stages',*a.changed_stages]
    if a.identical: command += ['--identical']
    with (a.out/'gate.txt').open('w') as f:
        result = subprocess.run(command,stdout=f,stderr=subprocess.STDOUT)
    print(f'GATE exit={result.returncode}: {a.out/"gate.txt"}',flush=True)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
