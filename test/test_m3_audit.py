"""Regression tests for missing-data and quality-gate decisions."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def run_pair(self, design='adaptec1', seed=1002, ratio=1.0, error=False, macro=False):
        for label, hpwl in [('baseline', 100.), ('candidate', 100. * ratio)]:
            run = self.root / label / 'ispd2005' / design / f'seed-{seed}'
            (run / design).mkdir(parents=True)
            (run / 'manifest.json').write_text(json.dumps(dict(
                aux=f'/data/ispd2005/{design}/{design}.aux',
                config=dict(random_seed=seed, target_density=1.,
                            target_overflow=.1, cgp_target_overflow=.1))))
            (run / 'completed.json').write_text('{}')
            (run / 'run.log').write_text('[ERROR] broken' if error and label == 'candidate' else '')
            (run / design / 'summary.json').write_text(json.dumps(dict(
                init_hpwl=10., gp_hpwl=hpwl, mgp_overflow=.05,
                gp_out_of_core=0, init_out_of_core=0, gp_movable_macros=int(macro))))
            (run / design / 'metrics.csv').write_text('stage,iter,overflow\nmGP,0,0.05\n')
            for stage in ['init', 'gp']:
                (run / design / f'{design}.{stage}.pl').write_text('layout\n')

    def gate(self, aggregation='paired-designs'):
        return subprocess.run(['python3', str(ROOT / 'scripts/m3_compare.py'),
            str(self.root / 'baseline'), str(self.root / 'candidate'), '--expected-count',
            str(len(list((self.root / 'baseline').glob('*/*/seed-1002/manifest.json')))),
            '--suite-aggregation', aggregation],
            capture_output=True, text=True)

    def test_identical_passes(self):
        self.run_pair()
        self.assertEqual(self.gate().returncode, 0)

    def test_missing_candidate_fails(self):
        self.run_pair()
        (self.root / 'candidate/ispd2005/adaptec1/seed-1002/completed.json').unlink()
        self.assertNotEqual(self.gate().returncode, 0)

    def test_error_fails(self):
        self.run_pair(error=True)
        self.assertIn('ERROR', self.gate().stdout)
        self.assertNotEqual(self.gate().returncode, 0)

    def test_outlier_needs_paired_seeds(self):
        self.run_pair(ratio=1.04)
        self.run_pair('adaptec2', ratio=.9)
        self.assertIn('five paired seeds required', self.gate().stdout)
        for seed in [1, 2, 3, 4]:
            self.run_pair(seed=seed)
        self.assertEqual(self.gate().returncode, 0)

    def test_partial_audit_does_not_pass_full_gate(self):
        self.run_pair()
        result = subprocess.run(['python3', str(ROOT / 'scripts/m3_compare.py'),
            str(self.root / 'baseline'), str(self.root / 'candidate')],
            capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('expected 32', result.stdout)

    def test_rollback_must_match_delivered_point(self):
        self.run_pair()
        out = self.root / 'candidate/ispd2005/adaptec1/seed-1002/adaptec1'
        path = out / 'summary.json'
        summary = json.loads(path.read_text())
        summary['mGP_rollback_iter'] = 1
        path.write_text(json.dumps(summary))
        (out / 'metrics.csv').write_text('stage,iter,overflow\nmGP,1,0.06\n')
        result = self.gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('rollback', result.stdout)

    def test_macro_requires_complete_five_seed_set(self):
        self.run_pair(macro=True)
        self.assertNotEqual(self.gate().returncode, 0)
        self.assertIn('five paired seeds incomplete', self.gate().stdout)

    def test_equal_design_aggregation_uses_fixed_five_only(self):
        self.run_pair(ratio=1.04, macro=True)
        for seed in [1, 2, 3, 4]:
            self.run_pair(seed=seed, ratio=.98, macro=True)
        self.assertEqual(self.gate().returncode, 0)
        self.assertNotEqual(self.gate('default-seed').returncode, 0)
        # A sixth seed is recorded/checked but must not dilute the fixed cohort.
        self.run_pair(seed=99, ratio=10., macro=True)
        self.assertEqual(self.gate().returncode, 0)

    def test_paired_outlier_fails_even_when_default_seed_and_suite_pass(self):
        self.run_pair(ratio=1.0, macro=True)
        self.run_pair('adaptec2', ratio=.8)
        for seed in [1, 2, 3, 4]:
            self.run_pair(seed=seed, ratio=1.1, macro=True)
        result = self.gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('paired five-seed HPWL degraded >3%', result.stdout)

    def test_nonfinite_intermediate_metrics_fail_even_with_finite_summary(self):
        self.run_pair()
        path = self.root / 'candidate/ispd2005/adaptec1/seed-1002/adaptec1/metrics.csv'
        path.write_text('stage,iter,overflow,lambda\nmGP,0,0.05,inf\n')
        result = self.gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('non-finite CSV', result.stdout)

    def test_independent_macro_gate_checks_frozen_macro_bounds(self):
        design = self.root / 'fixture'
        design.mkdir()
        (design / 'fixture.nodes').write_text('m 4 4\n')
        (design / 'fixture.scl').write_text(''.join(
            f'CoreRow Horizontal\n Coordinate : {y}\n Height : 1\n Sitewidth : 1\n'
            ' Sitespacing : 1\n SubrowOrigin : 0 NumSites : 20\nEnd\n'
            for y in range(20)))
        pl = design / 'result.pl'
        for x, expected in [(5, 0), (18, 1)]:
            pl.write_text(f'm {x} 5 : N /FIXED\n')
            result = subprocess.run(['python3', str(ROOT / 'scripts/check_overlap.py'),
                str(design), str(pl), '--gate'], capture_output=True, text=True)
            self.assertEqual(result.returncode, expected, result.stderr)


if __name__ == '__main__':
    unittest.main()
