"""Reproduce a poisoned native-tool history using an isolated local server.

The payload must come from the LocalCode regression fixture, not a private chat.
No model tools are executed. The script owns and stops only its server process.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import urllib.error
import urllib.request

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--server', type=Path, required=True)
p.add_argument('--model', type=Path, required=True)
p.add_argument('--payload', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
server, model = args.server.resolve(strict=True), args.model.resolve(strict=True)
args.output.mkdir(parents=True, exist_ok=False)
safe = json.loads(args.payload.read_text('utf8'))
safe.update(stream=False, max_tokens=16, temperature=0, seed=42,
            chat_template_kwargs={'enable_thinking': False})
bad = json.loads(json.dumps(safe))
bad['messages'][1]['tool_calls'].append({'id': 'broken', 'type': 'function',
    'function': {'name': 'list_directory', 'arguments': '{"path":"' + 'x' * 20376}})
bad['messages'].insert(3, {'role': 'tool', 'tool_call_id': 'broken', 'name': 'list_directory',
                           'content': 'ERROR: malformed arguments; nothing executed.'})
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
env = dict(os.environ)
for key in list(env):
    if key.startswith(('GGML_', 'LLAMA_ARG_')):
        env.pop(key)
command = [str(server), '--model', str(model), '--host', '127.0.0.1', '--port', str(port),
    '--ctx-size', '32768', '--parallel', '1', '--device', 'none', '--n-gpu-layers', '0',
    '--cache-type-k', 'f16', '--cache-type-v', 'f16', '--threads', '8', '--jinja', '--no-webui']
(args.output / 'command.json').write_text(json.dumps(command, indent=2), 'utf8')
url = 'http://127.0.0.1:' + str(port)

def request(route, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    return urllib.request.urlopen(urllib.request.Request(url + route, data,
        {'Content-Type': 'application/json'}), timeout=60)

with (args.output / 'server.log').open('wb') as log:
    proc = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    try:
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise RuntimeError('Owned server exited')
            try:
                with request('/health') as response:
                    if response.status == 200:
                        break
            except OSError:
                pass
            time.sleep(.1)
        else:
            raise RuntimeError('Readiness timeout')
        try:
            with request('/v1/chat/completions', bad):
                raise AssertionError('Malformed historical arguments were accepted')
        except urllib.error.HTTPError as error:
            old_status = error.code
            explanation = error.read().decode('utf8')
        with request('/v1/chat/completions', safe) as response:
            new_status = response.status
            answer = json.loads(response.read())
        assert old_status == 500 and 'Failed to parse tool call arguments' in explanation
        assert new_status == 200 and 'choices' in answer
        report = {'before_http': old_status, 'after_http': new_status,
            'malformed_argument_bytes': 20385, 'same_server_process': True,
            'weights': model.name, 'context': 32768, 'cache_k': 'f16', 'cache_v': 'f16',
            'history': 'Synthetic fixture only; no tools executed.',
            'payload_sha256': hashlib.sha256(args.payload.read_bytes()).hexdigest(),
            'usage_after': answer.get('usage')}
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=15)
report['owned_process_stopped'] = proc.poll() is not None
(args.output / 'result.json').write_text(json.dumps(report, indent=2), 'utf8')
print(json.dumps(report, indent=2))
