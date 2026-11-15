"""Real CLI failures must preserve a valid failure summary, not a success PL."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

cli, source = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    for p in source.glob('tiny.*'):
        shutil.copy2(p, root / p.name)
    cfg = root / 'config.json'
    cfg.write_text(json.dumps(dict(plot=False, num_threads=1, gp_bin_dim=4,
                                  gp_max_iter=1, gp_stagnation_window=0)))
    nodes = root / 'tiny.nodes'
    lines = nodes.read_text().splitlines()
    index = next(i for i, line in enumerate(lines)
                 if len(line.split()) >= 3 and line.split()[0] == 'c1')
    original = lines[index].split()
    for bad, kind in [('2000', 'out_of_core'), ('nan', 'non_finite')]:
        fields = list(original); fields[1] = bad
        lines[index] = ' '.join(fields)
        nodes.write_text('\n'.join(lines) + '\n')
        out = root / ('output-' + bad)
        r = subprocess.run([str(cli), '--aux', str(root / 'tiny.aux'), '--stage', 'gp',
                            '--config', str(cfg), '--out', str(out)], capture_output=True, text=True)
        assert r.returncode != 0, r.stdout
        summary = json.loads((out / 'tiny/summary.json').read_text())
        assert summary['run_status'] == 'failed', summary
        assert summary['failure_kind'] == kind, summary
        assert not (out / 'tiny/tiny.gp.pl').exists()
    # A movable macro in a fully blocked core has no legal position. Normal
    # mGP nonconvergence is allowed, but residual mLG overlap is a fatal error.
    nodes.write_text((source / 'tiny.nodes').read_text().replace('t1 4 10 terminal', 't1 20 20 terminal'))
    (root / 'tiny.pl').write_text((source / 'tiny.pl').read_text()
        .replace('m1 12 0 : N /FIXED', 'm1 12 0 : N')
        .replace('t1 8 0 : N /FIXED', 't1 0 0 : N /FIXED'))
    out = root / 'blocked-core'
    r = subprocess.run([str(cli), '--aux', str(root / 'tiny.aux'), '--stage', 'gp',
                        '--config', str(cfg), '--out', str(out)], capture_output=True, text=True)
    assert r.returncode != 0, r.stdout
    summary = json.loads((out / 'tiny/summary.json').read_text())
    assert summary['failure_kind'] == 'macro_overlap', summary
    assert not (out / 'tiny/tiny.gp.pl').exists()
print('PASS real CLI failure summary')
