"""Extract stage and payload measurements without inferring semantic success.

Usage: python scripts/performance_metrics.py <trace-directory> [output.json]
Replay transactions and optional stage events are kept distinct. Raw prompts
remain in the explicitly enabled trace directory; this report contains counts
and durations only. Correctness and relevance require separate corpus grading.
"""
import collections
import hashlib
import json
import math
import pathlib
import statistics
import sys


def summary(values):
    if not values:
        return None
    ordered = sorted(values)
    return {"n": len(values), "p50": statistics.median(values),
            "p95": ordered[math.ceil(.95 * len(ordered)) - 1], "samples": values}


def read_lines(path):
    with path.open(encoding="utf-8-sig") as source:
        return [json.loads(line) for line in source if line.strip()]


def measure(path, relevant_sources=()):
    transactions = read_lines(path)
    event_path = path.with_suffix(".events.jsonl")
    events = read_lines(event_path) if event_path.exists() else []
    tools = [event for event in events if event["stage"] == "agent_event"
             and event["fields"].get("type") == "tool_call_started"]
    calls = collections.Counter()
    call_types = collections.Counter()
    for event in tools:
        fields = event["fields"]
        arguments = fields.get("arguments", "")
        try:
            arguments = json.dumps(json.loads(arguments), sort_keys=True, separators=(",", ":"))
        except (ValueError, TypeError):
            pass
        calls[(fields["tool"], arguments)] += 1
        call_types[fields["tool"]] += 1
    request_bytes = []
    system_bytes = []
    schema_bytes = []
    prefix_hashes = []
    for transaction in transactions:
        request = transaction["request"]
        messages = request.get("messages", [])
        system = "\n".join(message.get("content", "") for message in messages
                           if message["role"] == "system").encode()
        schemas = json.dumps(request.get("tools", []), ensure_ascii=False,
                             separators=(",", ":")).encode()
        system_bytes.append(len(system))
        schema_bytes.append(len(schemas))
        prefix_hashes.append(hashlib.sha256(system + schemas).hexdigest())
        request_bytes.append(len(json.dumps(request, ensure_ascii=False,
                                           separators=(",", ":")).encode()))
    stage_times = collections.defaultdict(list)
    for event in events:
        if "elapsed_ms" in event["fields"]:
            stage_times[event["stage"]].append(event["fields"]["elapsed_ms"])
    submitted = next((event["at_unix_ms"] for event in events
                      if event["stage"] == "task_submitted"), None)
    prepared = next((event["at_unix_ms"] for event in events
                     if event["stage"] == "http_chat_prepared"), None)
    model_starts = [event for event in events if event["stage"] == "model_call_started"]
    last_tool = None
    continuation_ms = []
    for event in events:
        if event["stage"] == "agent_event" and event["fields"].get("type") == "tool_completed":
            last_tool = event["at_unix_ms"]
        elif event["stage"] == "model_call_started" and last_tool is not None:
            continuation_ms.append(event["at_unix_ms"] - last_tool)
            last_tool = None
    counts = collections.Counter(event["stage"] for event in events)
    reads = []
    started_reads = {}
    first_relevant_ms = None
    tool_result_bytes = 0
    for event in events:
        fields = event["fields"]
        if event["stage"] == "agent_event" and fields.get("type") == "tool_call_started":
            if fields.get("tool") in ("read_file", "read_file_range"):
                try:
                    arguments = json.loads(fields.get("arguments", "{}"))
                except (ValueError, TypeError):
                    arguments = {}
                source = arguments.get("path", "").replace("\\", "/")
                read = {"tool": fields["tool"], "path": source,
                        "listed_relevant_source": source in relevant_sources if relevant_sources else None,
                        "semantic_relevance": "pending manual review"}
                reads.append(read)
                started_reads[fields.get("call_id")] = read
        if event["stage"] == "tool_result":
            tool_result_bytes += fields.get("result_bytes", 0)
            read = started_reads.get(fields.get("call_id"))
            if read is not None:
                read["ok"] = fields.get("ok")
                if read["listed_relevant_source"] and read["ok"] and first_relevant_ms is None and submitted:
                    first_relevant_ms = event["at_unix_ms"] - submitted
    return {"file": path.name, "model_calls": len(transactions),
            "agent_turns": sum(row["phase"] == "agent" for row in transactions),
            "finalizer_calls": sum(row["phase"].startswith("finalization") for row in transactions),
            "tool_calls": len(tools) if events else None,
            "duplicate_tool_calls": sum(count - 1 for count in calls.values()) if events else None,
            "tool_types": dict(call_types) if events else None,
            "request_serialized_bytes": request_bytes,
            "request_system_bytes": system_bytes, "request_tool_schema_bytes": schema_bytes,
            "unique_system_and_tool_prefixes": len(set(prefix_hashes)),
            "reported_prompt_tokens": [row["metrics"].get("prompt_tokens") for row in transactions],
            "reported_completion_tokens": [row["metrics"].get("completion_tokens") for row in transactions],
            "model_request_ms": summary([row["metrics"]["duration_ms"] for row in transactions]),
            "stage_ms": {key: summary(value) for key, value in stage_times.items()},
            "task_to_http_prepared_ms": prepared - submitted
                if prepared is not None and submitted is not None else None,
            "tool_to_next_model_call_ms": summary(continuation_ms),
            "profile_event_counts": dict(counts), "model_call_start_events": len(model_starts),
            "file_reads": reads, "tool_result_bytes_including_errors": tool_result_bytes if events else None,
            "time_to_first_successful_listed_source_ms": first_relevant_ms,
            "notes": ["Serialized trace request bytes exclude HTTP-only payload fields.",
                      "HTTP prepared is a client timestamp, not server receipt.",
                      "Missing stage events are unavailable, never zero.",
                      "The corpus path list is an anchor list, not a complete relevance classifier.",
                      "Correctness, relevance and verification require separate grading."]}


def main():
    directory = pathlib.Path(sys.argv[1])
    rows = [measure(path) for path in sorted(directory.glob("*.jsonl"))
            if not path.name.endswith(".events.jsonl")]
    report = {"schema": 1, "runs": rows}
    destination = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else directory / "metrics.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for row in rows:
        print(row["file"], "turns", row["agent_turns"], "tools", row["tool_calls"],
              "prompt tokens", row["reported_prompt_tokens"],
              "setup ms", row["stage_ms"].get("prompt_setup", {}).get("p50"))


if __name__ == "__main__":
    main()
