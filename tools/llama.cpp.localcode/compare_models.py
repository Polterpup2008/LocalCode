"""Compare named model/runtime pairs using identical, synthetic fixed-output workloads.

This measures the supplied combinations, not parameter count or broad coding quality.
Run qualify_runtime.py separately for each pair's API and answer checks.
"""
from __future__ import annotations

import argparse
import json
import platform
from pathlib import Path
import statistics
import subprocess

from benchmark_runtime import Server, digest, write_json


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--case', action='append', nargs=3, required=True,
                        metavar=('NAME', 'SERVER', 'MODEL'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--context', type=int, default=4096)
    parser.add_argument('--threads', type=int, default=8)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--gpu-layers', type=int, default=999)
    args = parser.parse_args()
    if args.context < 4096 or args.repetitions < 1:
        parser.error('Use at least 4096 context tokens and one repetition.')
    cases = {name: (Path(server).resolve(strict=True), Path(model).resolve(strict=True))
             for name, server, model in args.case}
    if len(cases) != len(args.case):
        parser.error('Case names must be unique.')
    args.output.mkdir(parents=True, exist_ok=False)
    contract = {'context': args.context, 'gpu_layers': args.gpu_layers, 'threads': args.threads,
                'batch': 512, 'ubatch': 128, 'cache_k': 'f16', 'cache_v': 'f16',
                'startup_timeout_s': 300, 'request_timeout_s': 300}
    order = [*cases, *reversed(cases)]
    manifest = {'contract': contract, 'order': order, 'repetitions': args.repetitions,
                'platform': platform.platform(), 'processor': platform.processor(), 'cases': {},
                'notes': 'Same text, context, precision, GPU layer request and output token budget. '
                         'Different tokenizers can give different input token counts. '
                         'Warm-up excluded; EOS ignored only for throughput. No cached prompts.'}
    for name, (server, model) in cases.items():
        manifest['cases'][name] = {'server': str(server), 'server_sha256': digest(server),
                                   'model': str(model), 'model_sha256': digest(model),
                                   'model_bytes': model.stat().st_size,
                                   'runtime_files': {file.name: digest(file)
                                       for file in server.parent.iterdir()
                                       if file.suffix.lower() in ('.dll', '.exe')}}
    try:
        manifest['gpu'] = subprocess.check_output(
            ['nvidia-smi', '--query-gpu=name,driver_version,memory.total', '--format=csv'],
            text=True, timeout=10).strip()
    except (OSError, subprocess.SubprocessError):
        pass
    write_json(args.output / 'manifest.json', manifest)
    source = '\n'.join(f'def item_{i:03d}(x): return x + {i}' for i in range(90))
    prompts = {'short': 'Complete this Python function.\ndef add(a, b):\n    return',
               'long': source + '\n# Implement total(x), the sum of item_000(x), item_045(x), and item_089(x).\n'}
    rows = []
    phases = []
    for phase, name in enumerate(order):
        server_path, model_path = cases[name]
        print(f'Phase {phase + 1}/{len(order)}: {name}', flush=True)
        with Server(server_path, model_path, contract, args.output / f'{phase:02d}-{name}') as server:
            server.complete('warmup', 'def multiply(a, b):\n    return', predict=16,
                            ignore_eos=True)
            phases.append({'phase': phase, 'case': name, 'startup_s': server.startup_s})
            for repetition in range(args.repetitions):
                for workload, prompt in prompts.items():
                    sample = server.complete(f'{workload}-{repetition}', prompt, predict=128,
                                             stream=True, ignore_eos=True)
                    response = sample['response']
                    timings = response.get('timings', {})
                    if timings.get('predicted_n') != 128:
                        raise RuntimeError('The server did not complete the fixed 128-token workload.')
                    row = {'phase': phase, 'case': name, 'workload': workload,
                           'repetition': repetition, 'wall_s': sample['wall_s'],
                           'ttft_s': sample['ttft_s'], 'timings': timings,
                           'request_sha256': sample['request_sha256']}
                    rows.append(row)
                    write_json(args.output / 'results.json', rows)
                    print(json.dumps(row), flush=True)
        observations = json.loads((server.output / 'observations.json').read_text())
        phases[-1]['peak_process_rss_bytes'] = max(
            (sum(p['rss_bytes'] for p in row['processes']) for row in observations), default=0)
        phases[-1]['gpu_samples'] = [row['gpu_csv'] for row in observations if row.get('gpu_csv')]
        write_json(args.output / 'phases.json', phases)
    summary = {}
    for name in cases:
        summary[name] = {}
        for workload in prompts:
            group = [row for row in rows if row['case'] == name and row['workload'] == workload]
            summary[name][workload] = {
                'samples': len(group),
                'median_decode_tokens_s': statistics.median(r['timings']['predicted_per_second'] for r in group),
                'median_prompt_tokens_s': statistics.median(r['timings']['prompt_per_second'] for r in group),
                'median_ttft_s': statistics.median(r['ttft_s'] for r in group),
                'median_wall_s': statistics.median(r['wall_s'] for r in group)}
        summary[name]['peak_process_rss_bytes'] = max(p['peak_process_rss_bytes'] for p in phases if p['case'] == name)
    write_json(args.output / 'summary.json', summary)
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
