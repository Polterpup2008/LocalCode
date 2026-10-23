"""Controls for claims made by the runtime comparison harness, not speed evidence."""
import copy
import unittest

from analyze_runtime import analyze
from qualify_runtime import check_code


def campaign(pairs=6, ratio=1.1):
    order = ['baseline', 'candidate', 'candidate', 'baseline'] * ((pairs + 1) // 2)
    order = order[:pairs * 2]
    rows = []
    for phase, variant in enumerate(order):
        for workload in ('short', 'long', 'cached'):
            duration = 100.0 if variant == 'baseline' else 100.0 / ratio
            rows.append({'phase': phase, 'variant': variant, 'workload': workload,
                         'repetition': 0, 'tokens': [1, 2, 3], 'content': 'same answer',
                         'request_sha256': 'synthetic-same-request', 'wall_s': duration / 1000,
                         'ttft_s': duration / 10000,
                         'timings': {'predicted_n': 3, 'predicted_ms': duration, 'prompt_n': 500}})
    return {'order': order}, rows


class ProtocolTests(unittest.TestCase):
    def test_positive_control(self):
        manifest, rows = campaign()
        self.assertTrue(analyze(manifest, rows)['performance_gate_passed'])

    def test_no_gain_and_regression_controls(self):
        for ratio in (1.0, 0.9):
            self.assertFalse(analyze(*campaign(ratio=ratio))['performance_gate_passed'])

    def test_repetitions_cannot_substitute_for_independent_processes(self):
        manifest, rows = campaign(pairs=2, ratio=2.0)
        self.assertFalse(analyze(manifest, rows)['performance_gate_passed'])

    def test_memory_gain_requires_equal_capacity_and_no_timing_regression(self):
        manifest, rows = campaign(ratio=1.0)
        memory = [{'phase': phase, 'variant': variant, 'file_virtual_bytes': 5000,
                   'rss_bytes': 1000 if variant == 'baseline' else 500}
                  for phase, variant in enumerate(manifest['order'])]
        result = analyze(manifest, rows, memory=memory)
        self.assertTrue(result['memory']['memory_gate_passed'])
        self.assertFalse(result['performance_gate_passed'])
        slower_manifest, slower_rows = campaign(ratio=0.9)
        self.assertFalse(analyze(slower_manifest, slower_rows, memory=memory)['memory']['memory_gate_passed'])
        memory[1]['file_virtual_bytes'] = 1000
        with self.assertRaises(ValueError):
            analyze(manifest, rows, memory=memory)

    def test_faster_unequal_work_is_rejected(self):
        manifest, original = campaign()
        mutations = (
            lambda r: r.update(tokens=[1, 2]),
            lambda r: r.update(tokens=[1, 2, 4]),
            lambda r: r.update(request_sha256='different'),
            lambda r: r['timings'].update(prompt_n=1),
            lambda r: r['timings'].update(predicted_n=1),
        )
        for mutate in mutations:
            rows = copy.deepcopy(original)
            mutate(rows[3])
            with self.assertRaises(ValueError):
                analyze(manifest, rows)

    def test_code_checker_catches_wrong_math_without_running_model_code(self):
        cases = [(2, 3, 5), (-2, 3, 1), (0, 0, 0)]
        self.assertTrue(check_code('def solve(a,b):\n return a+b', cases))
        self.assertFalse(check_code('def solve(a,b):\n return a-b', cases))
        self.assertFalse(check_code('import os\ndef solve(a,b):\n return a+b', cases))
        with self.assertRaises(ValueError):
            check_code('def solve(a,b):\n return dangerous()', cases)


if __name__ == '__main__':
    unittest.main()
