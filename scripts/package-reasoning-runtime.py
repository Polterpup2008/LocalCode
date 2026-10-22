"""Package the hash-qualified CPU common-library repair; never alter a source runtime."""
from pathlib import Path
import argparse, hashlib, json, shutil

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with Path(path).open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest().upper()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--stock-runtime', type=Path, required=True)
    parser.add_argument('--corrected-runtime', type=Path, required=True)
    parser.add_argument('--openssl-license', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    proof = ROOT / 'docs/performance/proofs/reasoning-server-20261017'
    receipt = json.loads((ROOT / 'docs/performance/raw/reasoning-server-20261017.json').read_text('utf8'))
    plan = json.loads((proof / 'plan.json').read_text('utf8'))
    stage = json.loads((proof / 'tls-stage.json').read_text('utf8'))
    assert receipt['native_repair_qualified'] and not args.output.exists()
    source_files = {name.lower(): digest for name, digest in plan['stock_files'].items()
                    if Path(name).suffix.lower() in ('.dll', '.exe')}
    for name, digest in source_files.items():
        assert sha(args.stock_runtime / name) == digest, name
    names = ['llama-common.dll', 'libssl-3-x64.dll', 'libcrypto-3-x64.dll']
    for name in names:
        assert sha(args.corrected_runtime / name) == stage['runtimes']['candidate'][name], name
    assert sha(args.corrected_runtime / names[0]) == receipt['dlls']['candidate']
    assert args.openssl_license.is_file() and 1000 < args.openssl_license.stat().st_size < 65536
    args.output.mkdir(parents=True)
    rows = []
    for name in names:
        target = args.output / name
        shutil.copyfile(args.corrected_runtime / name, target)
        rows.append({'name': name, 'sha256': sha(target), 'bytes': target.stat().st_size})
    manifest = {'format': 1, 'id': 'localcode.runtime-reasoning', 'architecture': 'windows-x64',
                'source_revision': 'cb300598d5f90189cb69d2702f4930aaf99d32a2',
                'source_url': 'https://github.com/ggml-org/llama.cpp/tree/cb300598d5f90189cb69d2702f4930aaf99d32a2',
                'qualification_manifest_sha256': receipt['manifest_sha256'],
                'original_core_sha256': source_files['llama.dll'],
                'original_common_sha256': source_files['llama-common.dll'],
                'base_sha256': source_files['ggml-base.dll'], 'ggml_sha256': source_files['ggml.dll'],
                'server_sha256': source_files['llama-server.exe'], 'source_files': source_files, 'files': rows,
                'scope': 'Optional b10655 Windows x64 CPU runtime copy; two public CPU models qualified. No GPU, tool-call, multimodal, TPS or RAM claim. Native budget key required for templates ignoring enable_thinking.'}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', 'utf8')
    patch = ROOT / 'docs/performance/proofs/reasoning-prefill-20261017/fixture/reasoning-prefill-b10655.patch'
    shutil.copyfile(patch, args.output / 'reasoning-prefill.patch')
    shutil.copyfile(ROOT / 'docs/performance/proofs/reasoning-server-20261017/LICENSE-llama.cpp', args.output / 'LICENSE')
    shutil.copyfile(args.openssl_license, args.output / 'OPENSSL-LICENSE.txt')
    (args.output / 'README.md').write_text('''# Optional native reasoning repair

This package replaces only llama-common.dll in a separate, verified copy of the
official b10655 Windows x64 CPU runtime. The original CPU backend dispatcher and
all CPU ISA variants are preserved. The model and context remain owner choices.
The original runtime is never modified. Setup requires an explicit unchecked
choice; it does not combine this copy with the separate RAM optimization.

The patch fixes premature completion of a forced reasoning-end token while the
generation prompt is being accepted. LocalCode and the Discord plugin send
reasoning_budget_tokens=0 when native reasoning is explicitly disabled on a
managed runtime advertising that capability. Existing servers may still ignore
the request; external providers do not receive the extra native field.

Qualified on two public CPU models with exact ordinary-token controls, zero and
positive budgets, JSON schema and SSE. This does not certify every model, GPU,
tool or multimodal request and establishes no tokens/second or RAM gain.

llama.cpp source and patch base:
https://github.com/ggml-org/llama.cpp/tree/cb300598d5f90189cb69d2702f4930aaf99d32a2
The unchanged source is MIT licensed; LICENSE and reasoning-prefill.patch are
included. The common DLL was built with MSVC14.44 /O2 /Ob2 /MD, C++17, and Jinja,
subprocess and TLS support. Native build/qualification recipes and digests are
in LocalCode docs/performance/proofs/reasoning-server-20261017.

The two OpenSSL3.6.4 dependencies are from MSYS2 UCRT64. Their exact binary
identities are in manifest.json; Apache2.0 terms are in OPENSSL-LICENSE.txt.
https://www.openssl.org/ and https://packages.msys2.org/package/mingw-w64-ucrt-x86_64-openssl
''', 'utf8')
    print(json.dumps({'output': str(args.output), 'files': len(rows), 'manifest_sha256': sha(args.output / 'manifest.json')}))


if __name__ == '__main__':
    main()
