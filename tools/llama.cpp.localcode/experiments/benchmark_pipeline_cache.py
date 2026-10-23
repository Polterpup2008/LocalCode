"""Small fixed-work startup study; never touches a user's server or driver cache."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import statistics
import subprocess
import time
import urllib.request
import urllib.error

import psutil

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--server', type=Path, required=True)
p.add_argument('--model', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--pairs', type=int, default=6)
p.add_argument('--device', required=True, help='Runtime device identifier from --list-devices')
args = p.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
cache_dir = out / 'Pipeline cache é'
server = args.server.resolve(strict=True)
model = args.model.resolve(strict=True)
policy = {'primary': 'process-start to first content token on the short request',
          'admission': 'At least 5% median paired improvement; paired confidence interval must exclude zero; no >5% median prefill/decode regression; same tokens/outputs/capacity/precision.',
          'warmup': 'One cache-disabled driver warmup and one first-use explicit cache run, both retained separately.',
          'controls': {'context': 32768, 'cache_k': 'f16', 'cache_v': 'f16', 'threads': 8,
                       'batch': 512, 'ubatch': 512, 'temperature': 0, 'seed': 42, 'max_tokens': 64},
          'model': model.name, 'model_sha256': hashlib.file_digest(model.open('rb'), 'sha256').hexdigest(),
          'backend_sha256': hashlib.file_digest((server.parent / 'ggml-vulkan.dll').open('rb'), 'sha256').hexdigest(),
          'driver_cache_changed': False, 'existing_user_server_stopped': False,
          'background': 'Run after compilation and other tests finish. Record other active workloads; never close user applications automatically.'}
(out / 'protocol.json').write_text(json.dumps(policy, indent=2), 'utf8')

def http(base, route, body=None, timeout=15):
    data = None if body is None else json.dumps(body).encode()
    return urllib.request.urlopen(urllib.request.Request(base + route, data,
        {'Content-Type': 'application/json'}), timeout=timeout)

def gpu_snapshot():
    r = subprocess.run(['nvidia-smi', '--query-gpu=memory.free,memory.total,utilization.gpu,temperature.gpu,clocks.sm',
                        '--format=csv,noheader,nounits'], capture_output=True, timeout=10,
                       creationflags=subprocess.CREATE_NO_WINDOW)
    return r.stdout.decode().strip()

def run_case(label, enabled):
    directory = out / label
    directory.mkdir()
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
    base = 'http://127.0.0.1:' + str(port)
    env = dict(os.environ)
    for key in list(env):
        if key.startswith(('GGML_', 'LLAMA_ARG_')): env.pop(key)
    if enabled: env['GGML_VK_PIPELINE_CACHE_DIR'] = str(cache_dir)
    command = [str(server), '--model', str(model), '--host', '127.0.0.1', '--port', str(port),
               '--ctx-size', '32768', '--parallel', '1', '--device', args.device, '--n-gpu-layers', '999',
               '--fit', 'off', '--cache-type-k', 'f16', '--cache-type-v', 'f16', '--flash-attn', 'on',
               '--batch-size', '512', '--ubatch-size', '512', '--threads', '8', '--threads-batch', '8',
               '--cache-ram', '64', '--jinja', '--no-webui', '--log-verbosity', '4']
    before_gpu = gpu_snapshot()
    log = (directory / 'server.log').open('wb')
    started = time.perf_counter()
    proc = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
        creationflags=subprocess.CREATE_NO_WINDOW)
    owned = psutil.Process(proc.pid)
    identity = owned.create_time()
    result = {'label': label, 'cache_enabled': enabled, 'gpu_before': before_gpu, 'requests': []}
    try:
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            if proc.poll() is not None: raise RuntimeError('Owned server exited: ' + label)
            try:
                with http(base, '/health', timeout=1) as response:
                    if response.status == 200: break
            except (OSError, urllib.error.HTTPError): pass
            time.sleep(.1)
        else: raise RuntimeError('Owned server did not become ready')
        result['startup_s'] = time.perf_counter() - started
        result['rss_at_ready'] = owned.memory_info().rss
        prompts = [
            'Write a concise Python function that returns the sum of a list of integers. Return only code.',
            '\n'.join(f'def helper_{i}(x): return x + {i}' for i in range(150)) +
            '\nWrite a short Python function that calls helper_0, helper_1 and helper_2 and returns their sum. Return only code.'
        ]
        for prompt in prompts:
            payload = {'model': 'local', 'messages': [{'role': 'user', 'content': prompt}],
                       'chat_template_kwargs': {'enable_thinking': False}, 'temperature': 0, 'seed': 42,
                       'top_k': 40, 'top_p': .95, 'min_p': .05, 'repeat_penalty': 1.05,
                       'max_tokens': 64, 'stream': True, 'stream_options': {'include_usage': True}, 'cache_prompt': False}
            sent = time.perf_counter()
            first = None
            content = ''
            usage, timings, finish = {}, {}, ''
            with http(base, '/v1/chat/completions', payload, timeout=180) as response:
                for raw in response:
                    if not raw.startswith(b'data: '): continue
                    data = raw[6:].strip()
                    if data == b'[DONE]': break
                    event = json.loads(data)
                    if 'error' in event: raise RuntimeError(str(event['error']))
                    usage = event.get('usage', usage)
                    timings = event.get('timings', timings)
                    for choice in event.get('choices', []):
                        text = choice.get('delta', {}).get('content') or ''
                        if text and first is None: first = time.perf_counter()
                        content += text
                        finish = choice.get('finish_reason') or finish
            if first is None or not usage or not timings: raise RuntimeError('Missing output/timing evidence')
            result['requests'].append({'prompt_sha256': hashlib.sha256(prompt.encode()).hexdigest(),
                'request_ttft_s': first - sent, 'process_ttft_s': first - started,
                'request_s': time.perf_counter() - sent, 'output': content,
                'usage': usage, 'timings': timings, 'finish_reason': finish})
        result['rss_after_requests'] = owned.memory_info().rss
    finally:
        # Kill only the exact owned process. This also tests persistence without
        # destructors: the application uses an owned Windows job for shutdown.
        if proc.poll() is None and owned.create_time() == identity: proc.kill()
        proc.wait(timeout=15)
        log.close()
        result['owned_process_stopped'] = proc.poll() is not None
        (directory / 'result.json').write_text(json.dumps(result, indent=2), 'utf8')
    text = (directory / 'server.log').read_text('utf8', errors='replace')
    placement = re.search(r'offloaded\s+(\d+)/(\d+) layers to GPU', text)
    if not placement or int(placement[1]) == 0 or placement[1] != placement[2] or 'n_ctx_slot = 32768' not in text:
        raise RuntimeError('Requested GPU placement/context not established')
    result['offloaded_layers'] = int(placement[1])
    result['total_layers'] = int(placement[2])
    result['cache_messages'] = [line for line in text.splitlines() if 'LocalCode pipeline cache' in line]
    if enabled and not list(cache_dir.glob('*.bin')): raise RuntimeError('No durable cache checkpoint')
    (directory / 'result.json').write_text(json.dumps(result, indent=2), 'utf8')
    print(label, 'startup', round(result['startup_s'], 3), 'first token',
          round(result['requests'][0]['process_ttft_s'], 3), flush=True)
    return result

cases = [run_case('driver-warmup-control', False), run_case('explicit-cache-first-use', True)]
for pair in range(args.pairs):
    for enabled in ([False, True] if pair % 2 == 0 else [True, False]):
        cases.append(run_case(f'pair-{pair + 1}-' + ('cache' if enabled else 'control'), enabled))
(out / 'results.json').write_text(json.dumps({'protocol': policy, 'cases': cases}, indent=2), 'utf8')
print('Campaign complete; admission requires a separate correctness and paired-statistics review.', flush=True)
