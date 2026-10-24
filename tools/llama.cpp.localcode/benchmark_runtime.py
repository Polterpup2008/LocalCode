"""Run matched local server workloads. Uses synthetic inputs, never chat history.

Python 3.11+, psutil, and nvidia-smi (optional for GPU observations).
The manifest freezes capacity/precision/sampling; timings never change them.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import threading
import time
import urllib.error
import urllib.request
import uuid

try:
    import psutil
except ImportError:
    psutil = None


def digest(path: Path) -> str:
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def write_json(path: Path, value) -> None:
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', 'utf8')
    temporary.replace(path)


class Server:
    def __init__(self, executable: Path, model: Path, contract: dict, output: Path):
        if psutil is None:
            raise RuntimeError('Live runtime tests require psutil: python -m pip install psutil')
        self.executable, self.model, self.contract, self.output = executable, model, contract, output
        self.process = None
        self.observations = []
        self.owned = {}
        self.stop = threading.Event()
        self.alias = 'localcode-bench-' + uuid.uuid4().hex
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            self.port = listener.getsockname()[1]
        self.url = f'http://127.0.0.1:{self.port}'

    def request(self, route: str, payload=None, *, timeout=None):
        data = None if payload is None else json.dumps(payload).encode('utf8')
        request = urllib.request.Request(self.url + route, data,
                                         {'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=timeout or self.contract['request_timeout_s']) as reply:
            return json.load(reply)

    def observe(self):
        root = psutil.Process(self.process.pid)
        psutil.cpu_percent()
        while not self.stop.is_set():
            row = {'elapsed_s': time.perf_counter() - self.start, 'processes': []}
            try:
                for proc in [root, *root.children(recursive=True)]:
                    self.owned[(proc.pid, proc.create_time())] = proc
                    with proc.oneshot():
                        memory, cpu = proc.memory_info(), proc.cpu_times()
                        row['processes'].append({'pid': proc.pid, 'rss_bytes': memory.rss,
                                                 'private_bytes': getattr(memory, 'private', None),
                                                 'cpu_s': cpu.user + cpu.system})
                row['host_cpu_percent'] = psutil.cpu_percent()
                row['host_available_bytes'] = psutil.virtual_memory().available
                if int(row['elapsed_s']) % 2 == 0:
                    gpu = subprocess.run(['nvidia-smi', '--query-gpu=temperature.gpu,power.draw,'
                                          'clocks.sm,utilization.gpu,memory.used,'
                                          'clocks_event_reasons.sw_thermal_slowdown',
                                          '--format=csv,noheader'], capture_output=True,
                                         text=True, timeout=5,
                                         creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
                    row['gpu_csv'] = gpu.stdout.strip()
            except (psutil.Error, FileNotFoundError, subprocess.TimeoutExpired) as error:
                row['observation_error'] = str(error)
            self.observations.append(row)
            self.stop.wait(0.5)

    def __enter__(self):
        self.output.mkdir(parents=True, exist_ok=False)
        self.log = (self.output / 'server.log').open('wb')
        c = self.contract
        self.command = [str(self.executable), '--model', str(self.model), '--host', '127.0.0.1',
                        '--port', str(self.port), '--alias', self.alias, '--ctx-size', str(c['context']),
                        '--parallel', '1', '--batch-size', str(c['batch']), '--ubatch-size', str(c['ubatch']),
                        '--threads', str(c['threads']), '--threads-batch', str(c['threads']),
                        '--n-gpu-layers', str(c['gpu_layers']), '--flash-attn', 'on',
                        '--cache-type-k', c['cache_k'], '--cache-type-v', c['cache_v'],
                        '--fit', 'off', '--no-context-shift']
        if c.get('mlock'):
            self.command.extend(['--load-mode', 'mmap+mlock'])
        elif c.get('load_mode'):
            self.command.extend(['--load-mode', c['load_mode']])
        if c.get('device'):
            self.command.extend(['--device', c['device']])
        self.start = time.perf_counter()
        self.process = subprocess.Popen(self.command, stdin=subprocess.DEVNULL, stdout=self.log,
                                        stderr=subprocess.STDOUT, cwd=self.executable.parent,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        self.monitor = threading.Thread(target=self.observe, daemon=True)
        self.monitor.start()
        try:
            while time.perf_counter() - self.start < c['startup_timeout_s']:
                if self.process.poll() is not None:
                    raise RuntimeError(f'Server exited at startup: {self.process.returncode}')
                try:
                    health = self.request('/health', timeout=2)
                    models = self.request('/v1/models', timeout=2)
                    if health.get('status') == 'ok' and any(m['id'] == self.alias for m in models['data']):
                        props = self.request('/props', timeout=2)
                        actual_context = props['default_generation_settings']['n_ctx']
                        write_json(self.output / 'props.json', props)
                        if actual_context != c['context']:
                            raise RuntimeError(f'Context capacity changed: {actual_context} != {c["context"]}')
                        self.startup_s = time.perf_counter() - self.start
                        write_json(self.output / 'launch.json', {'command': self.command,
                                   'startup_s': self.startup_s, 'pid': self.process.pid,
                                   'actual_context': actual_context})
                        return self
                except (urllib.error.URLError, TimeoutError):
                    pass
                time.sleep(0.1)
            raise TimeoutError('Startup timed out')
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        self.stop.set()
        if self.process:
            try:
                root = psutil.Process(self.process.pid)
                for proc in [root, *root.children(recursive=True)]:
                    self.owned[(proc.pid, proc.create_time())] = proc
            except psutil.Error:
                pass
            processes = []
            for (pid, created), proc in reversed(list(self.owned.items())):
                try:
                    if proc.create_time() == created and proc.is_running():
                        proc.terminate()
                        processes.append(proc)
                except psutil.Error:
                    pass
            _, alive = psutil.wait_procs(processes, timeout=5)
            for proc in alive:
                try:
                    proc.kill()
                except psutil.Error:
                    pass
            _, alive = psutil.wait_procs(alive, timeout=5)
            self.process.wait(timeout=5)
            self.monitor.join(timeout=7)
            self.log.close()
            write_json(self.output / 'observations.json', self.observations)
            write_json(self.output / 'cleanup.json', {'remaining_owned_pids': [p.pid for p in alive]})
            if alive:
                raise RuntimeError('Owned server processes did not stop')

    def complete(self, name: str, prompt: str, *, cached=False, predict=128, stream=False):
        payload = {'prompt': prompt, 'n_predict': predict, 'temperature': 0.0, 'seed': 42,
                   'top_k': 40, 'top_p': 0.95, 'min_p': 0.05, 'repeat_penalty': 1.05,
                   'cache_prompt': cached, 'return_tokens': True, 'stream': stream}
        write_json(self.output / (name + '.request.json'), payload)
        start, first = time.perf_counter(), None
        if stream:
            request = urllib.request.Request(self.url + '/completion', json.dumps(payload).encode(),
                                             {'Content-Type': 'application/json'})
            chunks = []
            with urllib.request.urlopen(request, timeout=self.contract['request_timeout_s']) as response:
                for line in response:
                    if line.startswith(b'data: '):
                        chunk = json.loads(line[6:])
                        if 'error' in chunk:
                            raise RuntimeError(str(chunk['error']))
                        if first is None and chunk.get('content'):
                            first = time.perf_counter() - start
                        chunks.append(chunk)
            if not chunks or not chunks[-1].get('stop'):
                raise RuntimeError('Incomplete SSE response')
            result = dict(chunks[-1])
            result['content'] = ''.join(chunk.get('content', '') for chunk in chunks)
            result['tokens'] = [token for chunk in chunks for token in chunk.get('tokens', [])]
            write_json(self.output / (name + '.chunks.json'), chunks)
        else:
            result = self.request('/completion', payload)
        row = {'name': name, 'wall_s': time.perf_counter() - start, 'ttft_s': first,
               'request_sha256': hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest(),
               'response': result}
        write_json(self.output / (name + '.json'), row)
        return row


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--runtime', action='append', required=True, metavar='NAME=SERVER')
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--order', default='baseline,candidate,candidate,baseline')
    parser.add_argument('--context', type=int, default=32768)
    parser.add_argument('--gpu-layers', type=int, default=999)
    parser.add_argument('--device', help='Explicit llama.cpp device, for example Vulkan0 or Vulkan1')
    parser.add_argument('--threads', type=int, default=8)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--cached-repetitions', type=int,
                        help='Override cached-request repetitions without repeating expensive cold prefills')
    parser.add_argument('--long-items', type=int, default=900,
                        help='Synthetic long-prompt function count; 1800 exercises about 30K tokens')
    parser.add_argument('--workloads', nargs='+', choices=('short', 'long', 'cached'),
                        default=['short', 'long', 'cached'])
    parser.add_argument('--load-mode', choices=('mmap', 'mmap+mlock', 'none'), default='mmap')
    parser.add_argument('--measure-residency', action='store_true',
                        help='Record native Windows mapped-file pages before and after workloads, outside timed requests')
    args = parser.parse_args()
    if args.cached_repetitions is None:
        args.cached_repetitions = args.repetitions
    if args.context < 16384:
        parser.error('This corpus requires at least 16K context; capacity is never reduced to improve a result.')
    runtimes = {name: Path(path).resolve(strict=True) for name, path in
                (item.split('=', 1) for item in args.runtime)}
    order = args.order.split(',')
    if set(order) - runtimes.keys():
        parser.error('Every order entry must name a runtime')
    args.model = args.model.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    contract = {'context': args.context, 'gpu_layers': args.gpu_layers, 'threads': args.threads,
                'batch': 512, 'ubatch': 512, 'cache_k': 'f16', 'cache_v': 'f16',
                'startup_timeout_s': 150, 'request_timeout_s': 240,
                'load_mode': args.load_mode,
                'model_sha256': digest(args.model), 'model_bytes': args.model.stat().st_size}
    if args.device:
        contract['device'] = args.device
    manifest = {'contract': contract, 'model': str(args.model), 'order': order,
                'repetitions': args.repetitions, 'cached_repetitions': args.cached_repetitions,
                'long_items': args.long_items,
                'workloads': args.workloads, 'runtime_files': {}}
    for name, path in runtimes.items():
        manifest['runtime_files'][name] = {file.name: digest(file) for file in path.parent.iterdir()
                                          if file.suffix.lower() in ('.exe', '.dll')}
    write_json(args.output / 'manifest.json', manifest)
    rows = []
    memory_rows = []
    memory_after_rows = []
    # Source-like, deterministic public synthetic data. Each numbered value matters.
    source = '\n'.join(f'def item_{i:04d}(x): return x + {i}' for i in range(args.long_items))
    prompts = {'short': 'Complete this Python function.\ndef add(a, b):\n    return',
               'long': source + '\n# Implement item_total(x), the sum of item_0000(x), item_0450(x), and item_0899(x).\n'}
    for phase, name in enumerate(order):
        print(f'Phase {phase + 1}/{len(order)}: {name}', flush=True)
        with Server(runtimes[name], args.model, contract, args.output / f'{phase:02d}-{name}') as server:
            if args.measure_residency:
                from windows_memory import mapped_file_residency
                memory = psutil.Process(server.process.pid).memory_info()
                memory_rows.append({'phase': phase, 'variant': name, 'rss_bytes': memory.rss,
                                    'private_bytes': memory.private,
                                    **mapped_file_residency(server.process.pid, args.model)})
                write_json(args.output / 'memory.json', memory_rows)
            server.complete('warmup', 'def multiply(a,b):\n    return', predict=8)
            for repetition in range(args.repetitions):
                for workload, prompt in prompts.items():
                    if workload not in args.workloads:
                        continue
                    row = server.complete(f'{workload}-{repetition}', prompt, stream=True)
                    rows.append({'phase': phase, 'variant': name, 'workload': workload,
                                 'repetition': repetition, 'wall_s': row['wall_s'],
                                 'ttft_s': row['ttft_s'], 'timings': row['response'].get('timings'),
                                 'tokens': row['response'].get('tokens'),
                                 'content': row['response'].get('content'),
                                 'request_sha256': row['request_sha256']})
            for repetition in range(args.cached_repetitions if 'cached' in args.workloads else 0):
                row = server.complete(f'cached-{repetition}', prompts['long'], cached=True, stream=True)
                rows.append({'phase': phase, 'variant': name, 'workload': 'cached',
                             'repetition': repetition, 'wall_s': row['wall_s'], 'ttft_s': row['ttft_s'],
                             'timings': row['response'].get('timings'), 'tokens': row['response'].get('tokens'),
                             'content': row['response'].get('content'), 'request_sha256': row['request_sha256']})
            if args.measure_residency:
                memory = psutil.Process(server.process.pid).memory_info()
                memory_after_rows.append({'phase': phase, 'variant': name, 'rss_bytes': memory.rss,
                                          'private_bytes': memory.private,
                                          **mapped_file_residency(server.process.pid, args.model)})
                write_json(args.output / 'memory_after.json', memory_after_rows)
        write_json(args.output / 'results.json', rows)
    summary = {}
    for name in dict.fromkeys(order):
        summary[name] = {}
        for workload in dict.fromkeys(args.workloads):
            group = [row for row in rows if row['variant'] == name and row['workload'] == workload]
            summary[name][workload] = {'samples': len(group),
                                      'median_wall_s': statistics.median(row['wall_s'] for row in group),
                                      'median_ttft_s': statistics.median(row['ttft_s'] for row in group)}
    write_json(args.output / 'summary.json', summary)
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    main()
