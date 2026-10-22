"""Exercise the real doctor CLI's argument, path and storage UTF-8 boundaries."""
import argparse
import json
import os
import pathlib
import sqlite3
import struct
import subprocess
import uuid


def check(doctor, scratch):
    root = scratch.resolve() / ('doctor-unicode-' + uuid.uuid4().hex)
    root.mkdir(parents=True)
    assert root.resolve().is_relative_to(scratch.resolve())
    workspace = root / 'projet é 路径 🦍'
    workspace.mkdir()
    home = root / 'données é 路径 🦍'
    env = os.environ.copy()
    env['LOCALCODE_HOME'] = str(home)
    records = []

    def run(name, args, expected):
        proc = subprocess.run([str(doctor), *map(str, args)], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=60, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        root.joinpath(name + '.stdout').write_bytes(proc.stdout)
        text = proc.stdout.decode('utf-8')
        assert proc.returncode == 0, (name, proc.returncode, text)
        assert expected in text, (name, expected, text)
        records.append({'case': name, 'passed': True})

    phrase = 'Préserve "prêt" 🦍'
    run('argument', ['cmd', root, 'echo', phrase], phrase)
    file = workspace / 'réponse 路径 🦍.txt'
    file.write_text('owned Unicode fixture\n', encoding='utf-8')
    run('path', ['path', workspace, file, 'read'], 'ALLOWED')
    assert file.name.encode('utf-8') in root.joinpath('path.stdout').read_bytes()
    skills = workspace / 'procédure 路径 🦍'
    skills.mkdir()
    skills.joinpath('SKILL.md').write_text(
        '---\nid: unicode-fixture\nname: Unicode fixture\ndescription: Unicode path fixture\n'
        'triggers: [unicode]\n---\nRead the relevant owned files before describing their contents. '
        'Keep changes inside the selected workspace and report any checks that were not performed.\n',
        encoding='utf-8')
    run('skills', ['skills', workspace], 'unicode-fixture')

    def string(value):
        data = value.encode('utf-8')
        return struct.pack('<Q', len(data)) + data

    metadata = [('general.architecture', 'llama'), ('general.name', 'Unicode fixture')]
    data = struct.pack('<4sIQQ', b'GGUF', 3, 0, len(metadata))
    for key, value in metadata: data += string(key) + struct.pack('<I', 8) + string(value)
    data += b'\0' * ((-len(data)) % 32)
    model = workspace / 'modèle 路径 🦍.gguf'
    model.write_bytes(data)
    run('models', ['models', 'add', model], '1 imported')
    assert home.joinpath('data/localcode.sqlite3').is_file()
    with sqlite3.connect(home / 'data/localcode.sqlite3') as db:
        profile, = db.execute('SELECT profile_json FROM model_profiles').fetchone()
        assert pathlib.Path(json.loads(profile)['model_path']).resolve() == model.resolve()
        assert db.execute('PRAGMA integrity_check').fetchone()[0] == 'ok'
    report = {'passed': True, 'cases': records, 'model_path_and_home_persisted': True,
              'scope': 'CLI argument quotes/Unicode, non-ANSI workspace/file/skill/model/storage paths; no model inference.'}
    root.joinpath('result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'passed': True, 'cases': len(records), 'evidence': str(root)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--doctor', type=pathlib.Path, required=True)
    parser.add_argument('--scratch', type=pathlib.Path, required=True)
    args = parser.parse_args()
    check(args.doctor.resolve(), args.scratch)
