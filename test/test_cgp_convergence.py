"""A cGP policy must preserve convergence when the frozen macro refines the grid."""
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile

cli, fixture = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory() as tmp:
    result = subprocess.run([str(cli), '--aux', str(fixture/'fixture.aux'),
        '--config', str(fixture/'config.json'), '--stage', 'init,gp', '--out', tmp],
        capture_output=True, text=True)
    assert result.returncode == 0, result.stderr + result.stdout
    assert re.search(r'cGP: .*\(bin 32x32,', result.stdout), result.stdout
    summary = json.loads((Path(tmp)/'fixture/summary.json').read_text())
    assert summary['cGP_converged'] == 1 and summary['cgp_overflow'] < .1, summary
    assert summary['cgp_iterations'] < 2000, summary
    assert summary['cgp_macros_moved'] == summary['mlg_overlap_after'] == 0, summary
print('PASS cGP convergence with frozen macro')
