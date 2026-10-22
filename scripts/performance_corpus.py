"""Run paired, sequential real-model corpus tasks with preserved raw evidence.

This runner does not grade correctness or turn a model's completion into proof.
Write cases require a separate fixture for each variant; navigation uses the
same frozen source directory. No wall-time thresholds are used as a CI gate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

from performance_metrics import measure
from performance_fixtures import create


def source_hashes(root):
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()
            and path.relative_to(root).parts[0] not in ("build", ".localcode", ".git")
            and path.relative_to(root).parts[:2] != ("performance_fixture", "build")}


def check_fixture(root, log_path):
    commands = [["cmake", "-S", "performance_fixture", "-B", "build/corpus-check", "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=Release"],
                ["cmake", "--build", "build/corpus-check", "--parallel", "2"],
                ["ctest", "--test-dir", "build/corpus-check", "--output-on-failure"],
                [str(root / "build/corpus-check/corpus_check.exe")]]
    stages = []
    with log_path.open("w", encoding="utf-8") as log:
        for command in commands:
            result = subprocess.run(command, cwd=root, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, check=False)
            text = result.stdout.decode("utf-8", errors="replace")
            log.write(text)
            stages.append({"command": command, "exit_code": result.returncode})
            if result.returncode:
                return {"passed": False, "stages": stages}
    return {"passed": text.strip() == "fixture: PASS", "stages": stages}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--doctor", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", action="append")
    parser.add_argument("--variant", choices=("off", "on"), action="append",
                        help="Run selected variants; default is the paired off/on comparison")
    parser.add_argument("--include-write", action="store_true")
    parser.add_argument("--context", type=int, default=32768)
    parser.add_argument("--time-budget", type=int, default=10,
                        help="Minutes per task, enforced by the production agent")
    parser.add_argument("--max-steps", type=int, default=16,
                        help="Production agent step limit; default preserves the paired benchmark")
    parser.add_argument("--corpus", type=Path, default=Path("docs/performance/corpus.json"))
    args = parser.parse_args()
    if not 4096 <= args.context <= 32768:
        parser.error("--context must be 4096 to 32768 for this corpus")
    if not 1 <= args.time_budget <= 480:
        parser.error("--time-budget must be 1 to 480 minutes")
    if not 1 <= args.max_steps <= 128:
        parser.error("--max-steps must be 1 to 128")
    args.output.mkdir(parents=True, exist_ok=False)
    corpus = json.loads(args.corpus.read_text(encoding="utf-8-sig"))
    selected = [case for case in corpus["cases"] if (case["kind"] == "read_only" or args.include_write)
                and (not args.case or case["id"] in args.case)]
    if not selected:
        parser.error("No implemented read-only cases selected")
    manifest = {"source_commit": corpus["source_commit"],
                "doctor_sha256": hashlib.sha256(args.doctor.read_bytes()).hexdigest(),
                "workspace": str(args.workspace.resolve()), "context": args.context,
                "time_budget_minutes": args.time_budget,
                "max_steps": args.max_steps, "order": args.variant or ["off", "on"], "runs": []}
    for case in selected:
        for variant in manifest["order"]:
            stem = f'{case["id"]}-{variant}'
            traces = args.output / (stem + "-traces")
            traces.mkdir()
            workspace = args.workspace.resolve()
            writing = case["kind"] == "write_fixture"
            baseline = before = None
            fixture_git_baseline = None
            if writing:
                workspace = create(workspace, (args.output / "workspaces" / stem).resolve(), case["id"])
                fixture_git_baseline = subprocess.check_output(
                    ["git", "rev-parse", "HEAD"], cwd=workspace, text=True).strip()
                before = source_hashes(workspace)
                baseline = check_fixture(workspace, args.output / (stem + "-baseline-check.log"))
                if baseline["passed"]:
                    raise RuntimeError("Coding fixture unexpectedly passed before agent work")
            environment = os.environ.copy()
            environment["LOCALCODE_TRACE_DIR"] = str(traces.resolve())
            # Dedicated application storage avoids changing the user's chats.
            environment["LOCALCODE_HOME"] = str((args.output / "home").resolve())
            command = [str(args.doctor.resolve()), "agent", str(workspace),
                       str(args.model.resolve()), case["prompt"], "--server",
                       str(args.server.resolve()), "--ctx", str(args.context), "--max-steps", str(args.max_steps),
                       "--time-budget", str(args.time_budget),
                       "--yes"]
            if not writing:
                command.append("--read-only")
            if variant == "off":
                command.append("--no-repo")
            print("Starting", stem, flush=True)
            start = time.monotonic()
            with (args.output / (stem + ".log")).open("w", encoding="utf-8") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                        env=environment, check=False)
            rows = [measure(path, case["relevant_sources"]) for path in sorted(traces.glob("*.jsonl"))
                    if not path.name.endswith(".events.jsonl")]
            run = {"case": case["id"], "variant": variant, "exit_code": result.returncode,
                   "wall_seconds_including_server": time.monotonic() - start,
                   "metrics": rows, "semantic_grade": "pending manual review"}
            if writing:
                after = source_hashes(workspace)
                changed = sorted(path for path in before.keys() | after.keys()
                                 if before.get(path) != after.get(path))
                run["source_changes"] = changed
                run["fixture_git_baseline"] = fixture_git_baseline
                run["scope_observed"] = set(changed) <= set(case["allowed_changes"])
                run["baseline_check"] = baseline
                run["independent_check"] = check_fixture(workspace, args.output / (stem + "-after-check.log"))
            manifest["runs"].append(run)
            (args.output / "results.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                                      encoding="utf-8")
            print("Completed", stem, "exit", result.returncode,
                  "turns/tools", [(row["agent_turns"], row["tool_calls"]) for row in rows],
                  flush=True)


if __name__ == "__main__":
    main()
