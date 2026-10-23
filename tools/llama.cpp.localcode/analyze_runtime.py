"""Reject unequal work and summarize paired process-level runtime comparisons."""
import argparse
import collections
import json
import math
from pathlib import Path
import random
import statistics

from benchmark_runtime import write_json


def analyze(manifest, rows, reference='baseline', candidate='candidate', memory=None):
    order = manifest['order']
    if len(order) % 2:
        raise ValueError('Comparison requires adjacent baseline/candidate process pairs')
    phases = collections.defaultdict(list)
    for row in rows:
        timings = row.get('timings') or {}
        if len(row['tokens']) != timings.get('predicted_n') or not row['tokens']:
            raise ValueError('Missing/inconsistent generated-token accounting')
        if timings.get('predicted_ms', 0) <= 0:
            raise ValueError('Missing generation duration')
        phases[row['phase']].append(row)
    comparisons = collections.defaultdict(list)
    for phase in range(0, len(order), 2):
        if {order[phase], order[phase + 1]} != {reference, candidate}:
            raise ValueError('Each process pair must contain both variants')
        ref_phase = phase if order[phase] == reference else phase + 1
        new_phase = phase + 1 if order[phase] == reference else phase
        ref = {(r['workload'], r['repetition']): r for r in phases[ref_phase]}
        new = {(r['workload'], r['repetition']): r for r in phases[new_phase]}
        if not ref or ref.keys() != new.keys():
            raise ValueError('Missing or unequal workloads')
        for key in ref:
            before, after = ref[key], new[key]
            if before['variant'] != reference or after['variant'] != candidate:
                raise ValueError('Row variant disagrees with process manifest')
            if before['request_sha256'] != after['request_sha256']:
                raise ValueError('Request inputs/settings differ')
            if before['tokens'] != after['tokens'] or before['content'] != after['content']:
                raise ValueError('Custom candidate changed generated tokens/content')
            if before['timings']['prompt_n'] != after['timings']['prompt_n']:
                raise ValueError('Different amount of prompt work/cache reuse')
        for workload in {key[0] for key in ref}:
            group = [key for key in ref if key[0] == workload]
            ref_ms = sum(ref[key]['timings']['predicted_ms'] for key in group)
            new_ms = sum(new[key]['timings']['predicted_ms'] for key in group)
            wall_ratio = sum(ref[key]['wall_s'] for key in group) / sum(new[key]['wall_s'] for key in group)
            ttft_ratio = sum(ref[key]['ttft_s'] for key in group) / sum(new[key]['ttft_s'] for key in group)
            comparisons[workload].append({'decode_ratio': ref_ms / new_ms,
                                          'wall_ratio': wall_ratio, 'ttft_ratio': ttft_ratio})
    rng = random.Random(20261023)
    summary = {}
    for workload, pairs in sorted(comparisons.items()):
        logs = [math.log(pair['decode_ratio']) for pair in pairs]
        bootstraps = sorted(math.exp(statistics.mean(rng.choices(logs, k=len(logs)))) - 1
                            for _ in range(10000))
        median = statistics.median(pair['decode_ratio'] for pair in pairs) - 1
        summary[workload] = {
            'independent_process_pairs': len(pairs), 'pairs': pairs,
            'median_decode_gain_percent': median * 100,
            'paired_bootstrap_95_percent': [100 * bootstraps[250], 100 * bootstraps[9749]],
            'median_wall_gain_percent': 100 * (statistics.median(p['wall_ratio'] for p in pairs) - 1),
            'median_ttft_gain_percent': 100 * (statistics.median(p['ttft_ratio'] for p in pairs) - 1),
        }
    enough = summary and all(row['independent_process_pairs'] >= 6 for row in summary.values())
    decode_gain = any(row['median_decode_gain_percent'] >= 5 and row['paired_bootstrap_95_percent'][0] > 0
                      for row in summary.values())
    no_regression = all(min(row['median_decode_gain_percent'], row['median_wall_gain_percent'],
                            row['median_ttft_gain_percent']) >= -5 for row in summary.values())
    result = {'workloads': summary, 'performance_gate_passed': bool(enough and decode_gain and no_regression),
            'sufficient_process_pairs': bool(enough), 'matched_tokens_and_requests': True,
            'no_observed_median_regression_over_5_percent': bool(no_regression),
            'limits': 'Process-pair bootstrap on this machine and observed load only. '
                      'Backend/API correctness and hardware coverage remain separate gates.'}
    if memory is not None:
        by_phase = {row['phase']: row for row in memory}
        if len(by_phase) != len(order) or len(memory) != len(order):
            raise ValueError('Memory comparison requires one native snapshot per process')
        savings = []
        for phase in range(0, len(order), 2):
            ref_phase = phase if order[phase] == reference else phase + 1
            new_phase = phase + 1 if order[phase] == reference else phase
            before, after = by_phase[ref_phase], by_phase[new_phase]
            if before['variant'] != reference or after['variant'] != candidate:
                raise ValueError('Memory variant disagrees with process manifest')
            if before['file_virtual_bytes'] <= 0 or before['file_virtual_bytes'] != after['file_virtual_bytes']:
                raise ValueError('Mapped-file capacity changed or model mapping is missing')
            if min(before['rss_bytes'], after['rss_bytes']) <= 0:
                raise ValueError('Missing process working set')
            savings.append(1 - after['rss_bytes'] / before['rss_bytes'])
        saving = statistics.median(savings)
        result['memory'] = {'median_process_working_set_reduction_percent': saving * 100,
                            'process_pair_reduction_percent': [x * 100 for x in savings],
                            'gate': 'At least 20% working-set reduction, six process pairs, exact work, '
                                    'and no observed median timing regression over 5%; no speed gain required.',
                            'memory_gate_passed': bool(enough and saving >= 0.20 and no_regression)}
    return result


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    manifest = json.loads((args.directory / 'manifest.json').read_text('utf8'))
    rows = json.loads((args.directory / 'results.json').read_text('utf8'))
    memory_path = args.directory / 'memory.json'
    memory = json.loads(memory_path.read_text('utf8')) if memory_path.exists() else None
    result = analyze(manifest, rows, memory=memory)
    write_json(args.directory / 'analysis.json', result)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
