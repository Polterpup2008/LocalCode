#!/usr/bin/env python3
"""Prepare/build the unselected Windows copied-mmap-page experiment in a new tree.

This is a research helper. The first native study saves working-set memory but
fails its throughput gate. It never installs a runtime or changes an owner profile.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import zipfile

REVISION = 'cb300598d5f90189cb69d2702f4930aaf99d32a2'
ARCHIVE_SHA256 = '07B8DD11BE9613458E95B6FFD0F30CFFB6E815980E93BE8DAB8A476C860877D9'
PATCH_SHA256 = '66177B4C55CCBCC54DCB8E93879A495B139E79CC1E1890CF813848DAD9D54580'
PATCHED_FILES = {
    'src/llama-model.cpp': 'C2607E44EB1B108725F338B91E48650C9A78AD21C664155412D70A82CFAB70A1',
    'src/llama-model-loader.cpp': 'E684B07DF3C9D7A71EF5FDE922F086DE5F57E38DF5E4BD804AF8E454D104E1E1',
    'src/mapping-trim-cpu.inc': '658E7C1187E6AAEB357EF1B9E08408D8A35C3B1281A3DC899963904C21859D99',
    'src/mapped-copy-pages.h': '6A45DD11C680D0BF961D1B3B269770A49B737798D1829A81D35B91432BC486B1',
}
PATCH = Path(__file__).resolve().parents[1] / 'docs/performance/proofs/windows-mmap-lifetime-20261015/windows-copied-mmap-b10655.patch'


def sha(path):
    with path.open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest().upper()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-zip', required=True, type=Path, help='Pinned git-archive ZIP; never modified')
    parser.add_argument('--output', required=True, type=Path, help='New directory; existing paths are refused')
    parser.add_argument('--patch', type=Path, default=PATCH)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('cmake_args', nargs=argparse.REMAINDER, help='Explicit compiler/dependency options after --')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    output = args.output.resolve()
    if output.exists():
        parser.error('--output must not exist; previous work is never replaced')
    archive = args.source_zip.resolve(strict=True)
    patch = args.patch.resolve(strict=True)
    if sha(archive) != ARCHIVE_SHA256:
        parser.error('--source-zip must be the pinned git-archive ZIP')
    if patch.stat().st_size > 32768 or hashlib.sha256(patch.read_text('utf-8').encode()).hexdigest().upper() != PATCH_SHA256:
        parser.error('--patch must be the verified copied-mmap-page patch')
    output.mkdir(parents=True)
    source = output / 'source'
    source.mkdir()
    with zipfile.ZipFile(archive) as bundle:
        for entry in bundle.infolist():
            relative = PurePosixPath(entry.filename)
            if (relative.is_absolute() or '..' in relative.parts or ':' in entry.filename or '\\' in entry.filename
                    or stat.S_ISLNK(entry.external_attr >> 16) or not (source / entry.filename).resolve().is_relative_to(source)):
                raise RuntimeError('Unexpected archive path: ' + entry.filename)
        bundle.extractall(source)
    env = os.environ.copy()
    env['GIT_CEILING_DIRECTORIES'] = str(output)
    apply = ['git', 'apply', '--unsafe-paths', '--directory=' + source.as_posix(), str(patch)]
    subprocess.run(apply[:2] + ['--check'] + apply[2:], cwd=source, env=env, check=True)
    subprocess.run(apply, cwd=source, env=env, check=True)
    for name, expected in PATCHED_FILES.items():
        actual = hashlib.sha256((source / name).read_text('utf-8').encode()).hexdigest().upper()
        if actual != expected:
            raise RuntimeError('Patched-source verification failed: ' + name)
    extra = args.cmake_args[1:] if args.cmake_args[:1] == ['--'] else args.cmake_args
    configure = [args.cmake, '-S', str(source), '-B', str(output / 'build'), '-DCMAKE_BUILD_TYPE=Release',
                 '-DBUILD_SHARED_LIBS=ON', '-DGGML_BACKEND_DL=ON', '-DGGML_CPU_ALL_VARIANTS=ON',
                 '-DGGML_NATIVE=OFF', '-DGGML_CUDA=OFF', '-DLLAMA_BUILD_NUMBER=10655',
                 '-DLLAMA_BUILD_COMMIT=' + REVISION[:9] + '-mmap-trim-experiment', *extra]
    build = [args.cmake, '--build', str(output / 'build'), '--config', 'Release', '--parallel', str(args.jobs), '--target', 'llama-server']
    record = {'status': 'prepared', 'revision': REVISION, 'source_archive_sha256': sha(archive),
              'patch_sha256': PATCH_SHA256, 'verified_patched_files': PATCHED_FILES, 'configure': configure, 'build': build,
              'experimental': True, 'selected_for_delivery': False, 'owner_settings_changed': False,
              'note': 'Initial working-set gain verified; initial throughput gate failed. No automatic installation/profile change. Full source build and other hosts require their own validation.'}
    manifest = output / 'candidate-build.json'
    def save():
        manifest.write_text(json.dumps(record, indent=2) + '\n', 'utf-8')
    save()
    if not args.prepare_only:
        try:
            subprocess.run(configure, env=env, check=True)
            subprocess.run(build, env=env, check=True)
            record['status'] = 'built'
        except Exception as error:
            record.update(status='failed', error=type(error).__name__ + ': ' + str(error))
            raise
        finally:
            save()
    print(json.dumps({'status': record['status'], 'manifest': str(manifest), 'experimental': True, 'selected_for_delivery': False}))


if __name__ == '__main__':
    main()
