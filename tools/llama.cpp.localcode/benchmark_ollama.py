"""Isolated Ollama comparison with the fixed LocalCode synthetic workload.

Never uses the user's Ollama daemon, profile, models, or chat history. Cross-engine
outputs may differ: this is a named-server comparison, not the custom-patch oracle.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import urllib.error
import urllib.request

from benchmark_runtime import Server, digest, write_json


class Ollama(Server):
    def __init__(self, executable, model, contract, output, storage):
        super().__init__(executable, model, contract, output)
        self.storage = storage
        self.alias = 'localcode-fixed-bench'

    def __enter__(self):
        self.output.mkdir(parents=True, exist_ok=False)
        self.storage.mkdir(parents=True, exist_ok=True)
        profile = self.storage / 'profile'
        profile.mkdir(exist_ok=True)
        self.environment = {key: value for key, value in os.environ.items()
                            if not key.upper().startswith('OLLAMA_')}
        isolated = {'OLLAMA_HOST': f'127.0.0.1:{self.port}',
                    'OLLAMA_MODELS': str(self.storage / 'models'),
                    'USERPROFILE': str(profile), 'HOME': str(profile),
                    'OLLAMA_NO_CLOUD': '1', 'OLLAMA_CONTEXT_LENGTH': str(self.contract['context']),
                    'OLLAMA_FLASH_ATTENTION': '1', 'OLLAMA_KV_CACHE_TYPE': 'f16',
                    'OLLAMA_NUM_PARALLEL': '1', 'OLLAMA_MAX_LOADED_MODELS': '1',
                    'OLLAMA_KEEP_ALIVE': '-1'}
        self.environment.update(isolated)
        self.command = [str(self.executable), 'serve']
        self.log = (self.output / 'server.log').open('wb')
        self.start = time.perf_counter()
        self.process = subprocess.Popen(self.command, env=self.environment, cwd=self.executable.parent,
                                        stdin=subprocess.DEVNULL, stdout=self.log, stderr=subprocess.STDOUT,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        self.monitor = threading.Thread(target=self.observe, daemon=True)
        self.monitor.start()
        try:
            while time.perf_counter() - self.start < 30:
                if self.process.poll() is not None:
                    raise RuntimeError('Isolated Ollama exited at startup')
                try:
                    version = self.request('/api/version', timeout=2)
                    break
                except (urllib.error.URLError, TimeoutError):
                    time.sleep(0.1)
            else:
                raise TimeoutError('Isolated Ollama did not start')
            tags = self.request('/api/tags')['models']
            if not any(m['name'] == self.alias + ':latest' for m in tags):
                modelfile = self.storage / 'Modelfile'
                modelfile.write_text('FROM "' + str(self.model).replace('\\', '/') + '"\n', 'utf8')
                imported = subprocess.run([str(self.executable), 'create', self.alias, '-f', str(modelfile)],
                                          env=self.environment, capture_output=True, timeout=300,
                                          creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
                (self.output / 'import.log').write_bytes(imported.stdout + imported.stderr)
                imported.check_returncode()
            blob = self.storage / 'models/blobs' / ('sha256-' + self.contract['model_sha256'])
            if not blob.is_file() or digest(blob) != self.contract['model_sha256']:
                raise RuntimeError('Ollama imported weights differ from the input GGUF')
            write_json(self.output / 'launch.json', {'command': self.command, 'environment': isolated,
                                                   'version': version, 'pid': self.process.pid,
                                                   'weights_sha256_verified': True})
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def complete(self, name, prompt, *, predict=128):
        payload = {'model': self.alias, 'prompt': prompt, 'raw': True, 'stream': True, 'keep_alive': -1,
                   'options': {'num_ctx': self.contract['context'], 'num_batch': 512, 'num_thread': 8,
                               'num_gpu': 999, 'num_predict': predict, 'temperature': 0.0, 'seed': 42,
                               'top_k': 40, 'top_p': 0.95, 'min_p': 0.05, 'repeat_penalty': 1.05,
                               'repeat_last_n': 64}}
        write_json(self.output / (name + '.request.json'), payload)
        request = urllib.request.Request(self.url + '/api/generate', json.dumps(payload).encode(),
                                         {'Content-Type': 'application/json'})
        start, first = time.perf_counter(), None
        chunks = []
        with urllib.request.urlopen(request, timeout=self.contract['request_timeout_s']) as response:
            for line in response:
                chunk = json.loads(line)
                if 'error' in chunk:
                    raise RuntimeError(chunk['error'])
                if first is None and chunk.get('response'):
                    first = time.perf_counter() - start
                chunks.append(chunk)
        if not chunks or not chunks[-1].get('done'):
            raise RuntimeError('Incomplete Ollama stream')
        result = chunks[-1]
        result['response'] = ''.join(chunk.get('response', '') for chunk in chunks)
        row = {'wall_s': time.perf_counter() - start, 'ttft_s': first, 'response': result,
               'prompt_sha256': hashlib.sha256(prompt.encode()).hexdigest()}
        write_json(self.output / (name + '.chunks.json'), chunks)
        write_json(self.output / (name + '.json'), row)
        return row


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--storage', type=Path, required=True)
    parser.add_argument('--phases', type=int, default=4)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    contract = {'context': 32768, 'request_timeout_s': 300, 'model_sha256': digest(args.model)}
    write_json(args.output / 'manifest.json', {'contract': contract, 'server_sha256': digest(args.server),
                                            'limits': 'Different engines can generate different outputs; '
                                            'record prompt/cache counts and do not infer kernel gains.'})
    source = '\n'.join(f'def item_{i:04d}(x): return x + {i}' for i in range(900))
    prompts = [('short', 'Complete this Python function.\ndef add(a, b):\n    return'),
               ('long', source + '\n# Implement item_total(x), the sum of item_0000(x), item_0450(x), and item_0899(x).\n')]
    rows = []
    for phase in range(args.phases):
        print(f'Ollama phase {phase + 1}/{args.phases}', flush=True)
        with Ollama(args.server.resolve(), args.model.resolve(), contract,
                    args.output / f'{phase:02d}-ollama', args.storage.resolve()) as server:
            server.complete('warmup', 'def multiply(a,b):\n    return', predict=8)
            loaded = server.request('/api/ps')
            write_json(server.output / 'loaded.json', loaded)
            actual = loaded['models']
            if len(actual) != 1 or actual[0].get('context_length') != 32768:
                raise RuntimeError('Ollama did not retain the fixed 32K context')
            for workload, prompt in [*prompts, ('cached', prompts[1][1])]:
                row = server.complete(workload, prompt)
                row.update(phase=phase, workload=workload)
                rows.append(row)
        write_json(args.output / 'results.json', rows)


if __name__ == '__main__':
    main()
