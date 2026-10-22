"""Prove corpus checkers reject their injected faults and accept a known repair.

Only disposable validation copies are modified. These are harness controls,
not agent-task results and must not be counted as successful coding tasks.
"""
import json
from pathlib import Path
import sys

from performance_corpus import check_fixture
from performance_fixtures import CASES


def main():
    root = Path(sys.argv[1]).resolve()
    results = {}
    for case_id, specification in CASES.items():
        workspace = root / case_id
        source = workspace / "src/core/util/Strings.cpp"
        text = source.read_text(encoding="utf-8")
        if "original" in specification:
            if text.count(specification["regressed"]) != 1:
                raise RuntimeError("Positive-control source is not the expected injected fixture")
            text = text.replace(specification["regressed"], specification["original"])
        else:
            header = workspace / "src/core/util/Strings.h"
            header.write_text(header.read_text(encoding="utf-8") +
                              "\nnamespace localcode::strings {\n"
                              "bool iends_with(std::string_view s, std::string_view suffix);\n}\n",
                              encoding="utf-8")
            text += "\nnamespace localcode::strings {\nbool iends_with(std::string_view s, " \
                    "std::string_view suffix) {\nreturn s.size() >= suffix.size() && " \
                    "iequals(s.substr(s.size() - suffix.size()), suffix);\n}\n}\n"
        source.write_text(text, encoding="utf-8")
        results[case_id] = check_fixture(workspace, root / (case_id + "-positive-control.log"))
    (root / "positive-control-results.json").write_text(json.dumps(results, indent=2) + "\n",
                                                        encoding="utf-8")
    print({name: result["passed"] for name, result in results.items()})
    if not all(result["passed"] for result in results.values()):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
