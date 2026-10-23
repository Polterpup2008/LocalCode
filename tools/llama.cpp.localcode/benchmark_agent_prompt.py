#!/usr/bin/env python3
"""Compare a synthetic greeting through two real LocalCode CLI builds.

Uses only fresh application homes and a synthetic workspace. No model download,
driver change, user profile, existing server or existing conversation is used.
This measures application request overhead, not identical-prompt kernel speed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import time


def run(command, env, log, timeout=210):
    started = time.monotonic()
    with log.open("w", encoding="utf-8") as out:
        result = subprocess.run([str(arg) for arg in command], env=env, stdout=out,
                                stderr=subprocess.STDOUT, timeout=timeout,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                                check=False)
    return result.returncode, time.monotonic() - started


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("baseline", "candidate", "server", "model", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--gpu-layers", type=int, default=-1)
    parser.add_argument("--context", type=int, default=32768)
    args = parser.parse_args()
    for name in ("baseline", "candidate", "server", "model"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    workspace = output / "synthetic-project"
    workspace.mkdir()
    (workspace / "README.md").write_text("Synthetic greeting benchmark. No task requested.\n", encoding="utf-8")
    (workspace / "calc.py").write_text("def add(a, b):\n    return a + b\n", encoding="utf-8")
    report = {"kind": "application-greeting-comparison", "context": args.context,
              "gpu_layers": args.gpu_layers, "generation_budget": 64,
              "model": args.model.name, "server": str(args.server),
              "model_sha256": hashlib.file_digest(args.model.open("rb"), "sha256").hexdigest(),
              "same_prompt_work": False,
              "limits": "One fresh process per build; no universal speed claim. Context, model, KV precision and sampling are held fixed. Actual prompt work and output length differ.",
              "cases": []}
    for label, binary in (("before", args.baseline), ("after", args.candidate)):
        case = output / label
        case.mkdir()
        env = os.environ.copy()
        env["LOCALCODE_HOME"] = str(case / "home")
        env["LOCALCODE_TRACE_DIR"] = str(case / "trace")
        code, _ = run([binary, "models", "add", args.model], env, case / "import.log")
        if code != 0:
            raise RuntimeError("Model import failed: " + str(case / "import.log"))
        database = case / "home" / "data" / "localcode.sqlite3"
        # This database was created above in our new case directory. Editing its
        # synthetic profile applies identical controls without altering the
        # installed application's settings or requiring new CLI flags in v1.0.
        with sqlite3.connect(database) as db:
            rows = db.execute("SELECT id, profile_json FROM model_profiles").fetchall()
            if len(rows) != 1:
                raise RuntimeError("Expected one synthetic model profile")
            model_id, encoded = rows[0]
            profile = json.loads(encoded)
            profile.update(context_size=args.context, gpu_layers=args.gpu_layers,
                           auto_gpu_layers=False, reasoning_enabled=False,
                           threads=8, threads_batch=8, batch_size=512, ubatch_size=512,
                           prompt_cache_mib=64, cache_type_k="f16", cache_type_v="f16")
            profile["sampling"].update(max_tokens=64, temperature=0, seed=42)
            db.execute("UPDATE model_profiles SET profile_json=? WHERE id=?",
                       (json.dumps(profile), model_id))
        command = [binary, "agent", workspace, args.model, "hello how are you",
                   "--server", args.server, "--ctx", str(args.context),
                   "--gpu-layers", str(args.gpu_layers), "--max-steps", "1",
                   "--time-budget", "3"]
        code, elapsed = run(command, env, case / "agent.log")
        stdout = (case / "agent.log").read_text(encoding="utf-8", errors="replace")
        events = []
        for path in (case / "trace").glob("*.events.jsonl"):
            events.extend(json.loads(line) for line in path.read_text(encoding="utf-8").splitlines())
        payloads = [entry["fields"]["payload"] for entry in events if entry["stage"] == "http_chat_prepared"]
        if not payloads:
            raise RuntimeError("No actual serialized request captured: " + str(case))
        payload = payloads[0]
        (case / "request.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        text = "\n".join(message.get("content", "") for message in payload["messages"])
        metrics = re.search(r"\[gen\].*?([0-9.]+) tok/s decode\s+([0-9.]+) tok/s prompt \((\d+) eval.*?ttft ([0-9.]+)s", stdout)
        entry = {"build": label, "exit_code": code, "process_wall_s": elapsed,
                 "binary_sha256": hashlib.file_digest(binary.open("rb"), "sha256").hexdigest(),
                 "tool_schemas": len(payload.get("tools", [])), "message_bytes": len(text.encode("utf-8")),
                 "payload_bytes": len(json.dumps(payload, separators=(",", ":")).encode("utf-8")),
                 "sampling": {key: payload.get(key) for key in ("max_tokens", "temperature", "seed")}}
        if metrics:
            entry.update(decode_tokens_s=float(metrics[1]), prompt_tokens_s=float(metrics[2]),
                         evaluated_prompt_tokens=int(metrics[3]), ttft_s=float(metrics[4]))
        context = re.search(r"\[context\].*?\(system (\d+), skills (\d+), task (\d+), tools (\d+)", stdout)
        if context:
            entry["component_estimates"] = dict(zip(("system", "skills", "task", "tools"), map(int, context.groups())))
        entry["acceleration"] = next((line.strip() for line in stdout.splitlines() if "acceleration" in line), "See owned server log; v1.0 did not print an offload summary.")
        report["cases"].append(entry)
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(entry), flush=True)
    if report["cases"][0]["sampling"] != report["cases"][1]["sampling"]:
        raise RuntimeError("Sampling controls did not match")


if __name__ == "__main__":
    main()
