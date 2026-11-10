#!/usr/bin/env python3
"""Paired mLG replay: both implementations read the very same mGP PL bytes."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', required=True, type=Path)
    p.add_argument('--baseline-bin', required=True, type=Path)
    p.add_argument('--candidate-bin', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args()
    failures = 0
    count = 0
    for manifest in sorted(a.source.glob('*/*/seed-*/manifest.json')):
        run = manifest.parent
        meta = json.loads(manifest.read_text())
        design = Path(meta['aux']).stem
        pl = run / design / f'{design}.mgp.pl'
        if not pl.exists():
            raise SystemExit(f'Missing mGP snapshot: {pl}')
        rel = run.relative_to(a.source)
        summaries = []
        for label, binary in [('baseline', a.baseline_bin), ('candidate', a.candidate_bin)]:
            out = a.out / rel / label
            out.mkdir(parents=True, exist_ok=True)
            with (out / 'run.log').open('w') as log:
                subprocess.run([str(binary.resolve()), meta['aux'], str(pl),
                                str(run / 'config.json'), str(out)],
                               stdout=log, stderr=subprocess.STDOUT, check=True,
                               env=dict(os.environ, OMP_NUM_THREADS='16', OMP_WAIT_POLICY='PASSIVE'))
            summaries.append(json.loads((out / 'summary.json').read_text()))
        b, n = summaries
        if 'mlg_final_disp_max' not in b:  # no movable macros
            continue
        count += 1
        improved = n['mlg_final_disp_max'] < b['mlg_final_disp_max']
        acceptable = (n['mlg_final_disp_mean'] <= b['mlg_final_disp_mean'] and
                      n['mlg_final_disp_max'] <= b['mlg_final_disp_max'] and n['overlap_after'] == 0)
        failures += not acceptable
        print(f'{rel}: max {b["mlg_final_disp_max"]:.3f} -> {n["mlg_final_disp_max"]:.3f}; '
              f'mean {b["mlg_final_disp_mean"]:.3f} -> {n["mlg_final_disp_mean"]:.3f}; '
              f'improved={improved} acceptable={acceptable}', flush=True)
    if not count:
        raise SystemExit('No movable-macro runs were compared')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
