import argparse
import json
from pathlib import Path
import random
import statistics

p = argparse.ArgumentParser()
p.add_argument('directory', type=Path)
args = p.parse_args()
data = json.loads((args.directory / 'results.json').read_text('utf8'))
cases = {r['label']: r for r in data['cases']}
pairs = []
equivalent = True
count = len([name for name in cases if name.startswith('pair-') and name.endswith('-control')])
if count != 6:
    raise ValueError('This preset statistical gate requires six pairs.')
for n in range(1, count + 1):
    control = cases[f'pair-{n}-control']
    cache = cases[f'pair-{n}-cache']
    comparisons = []
    for a, b in zip(control['requests'], cache['requests']):
        same = (a['prompt_sha256'] == b['prompt_sha256'] and a['output'] == b['output'] and
                a['usage']['prompt_tokens'] == b['usage']['prompt_tokens'] and
                a['usage']['completion_tokens'] == b['usage']['completion_tokens'])
        equivalent &= same
        comparisons.append({'same_work': same, 'prompt_tokens': a['usage']['prompt_tokens'],
            'output_tokens': a['usage']['completion_tokens'],
            'prefill_ratio': b['timings']['prompt_ms'] / max(a['timings']['prompt_ms'], .001),
            'decode_ratio': b['timings']['predicted_ms'] / max(a['timings']['predicted_ms'], .001)})
    before = control['requests'][0]['process_ttft_s']
    after = cache['requests'][0]['process_ttft_s']
    pairs.append({'pair': n, 'before_s': before, 'after_s': after,
                  'improvement': 1 - after / before, 'requests': comparisons})
gains = [r['improvement'] for r in pairs]
rng = random.Random(42)
sampled = sorted(statistics.median(rng.choices(gains, k=len(gains))) for _ in range(10000))
ci = [sampled[250], sampled[9749]]
no_regression = all(statistics.median(row['requests'][q][metric] for row in pairs) <= 1.05
    for q in (0, 1) for metric in ('prefill_ratio', 'decode_ratio'))
report = {'same_outputs_and_token_counts': equivalent, 'pairs': pairs,
    'median_primary_improvement': statistics.median(gains), 'paired_bootstrap_95pct_interval': ci,
    'no_over_5pct_prefill_or_decode_median_regression': no_regression,
    'admitted': equivalent and statistics.median(gains) >= .05 and ci[0] > 0 and no_regression,
    'scope': 'Qwen3.5-9B Q4_K_M Vulkan process startup on one NVIDIA laptop, warm OS/driver caches; no universal decode claim.'}
(args.directory / 'analysis.json').write_text(json.dumps(report, indent=2), 'utf8')
print(json.dumps(report, indent=2))
