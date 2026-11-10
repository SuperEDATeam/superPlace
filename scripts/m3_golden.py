#!/usr/bin/env python3
"""Generate/check a separate M3 (.10 target) golden set without changing legacy hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bin', required=True, type=Path)
    p.add_argument('--out', type=Path, default=Path('results/m3/golden'))
    p.add_argument('--update', action='store_true')
    a = p.parse_args()
    for name in ['thin1', 'adaptec1', 'mms_adaptec1']:
        aux, = (Path('test_data') / name).glob('*.aux')
        design = aux.stem
        hashes = []
        for threads in [1, 8, 32]:
            run = a.out / name / f't{threads}'
            run.mkdir(parents=True, exist_ok=True)
            cfg = run / 'config.json'
            cfg.write_text(json.dumps(dict(target_density=1., target_overflow=.1,
                cgp_target_overflow=.1, poisson_backend='cpu', num_threads=threads, plot=False)))
            with (run / 'run.log').open('w') as log:
                subprocess.run([str(a.bin.resolve()), '--aux', str(aux), '--stage', 'init,gp',
                    '--config', str(cfg), '--out', str(run), '--threads', str(threads),
                    '--no-plot'], check=True, stdout=log, stderr=subprocess.STDOUT)
            hashes.append(''.join(f'{hashlib.sha256((run / design / (design + "." + stage + ".pl")).read_bytes()).hexdigest()}  {design}.{stage}.pl\n'
                                  for stage in ['init', 'gp']))
            print(f'{name} t{threads} complete', flush=True)
        if len(set(hashes)) != 1:
            raise SystemExit(f'{name}: thread-dependent output; refusing golden update')
        expected = Path('test/golden/m3') / f'{name}.sha256'
        if a.update:
            expected.parent.mkdir(parents=True, exist_ok=True)
            expected.write_text(hashes[0])
        elif not expected.is_file() or expected.read_text() != hashes[0]:
            raise SystemExit(f'{name}: M3 golden mismatch')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
