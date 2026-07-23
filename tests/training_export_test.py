#!/usr/bin/env python3

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(exporter, history, *args, expected=0):
    result = subprocess.run(
        [sys.executable, str(exporter), "--history", str(history), *args],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    require(result.returncode == expected, f"unexpected exit {result.returncode}: {result.stderr}")
    return result


def main():
    require(len(sys.argv) == 2, "expected training exporter path")
    exporter = Path(sys.argv[1])
    require(exporter.is_file(), f"missing exporter: {exporter}")

    candidate_body = """protocol\t1
preedit\twoc
application\tSecretEditor
surrounding_before\tprivate-left
surrounding_after\tprivate-right
surrounding_features\tbefore:v1:11111111111111111111111111111111\tafter:v1:22222222222222222222222222222222
candidates\t我才\t我\t操
candidate_metadata\t0\tconsumed_prefix\t0\tsource\tlookup\tscore\t100
candidate_metadata\t1\tconsumed_prefix\t2\tsource\tprefix\tscore\t90
candidate_metadata\t2\tconsumed_prefix\t1\tsource\tprefix\tscore\t80
state\tpreedit_cursor\t3\tcandidate_cursor\t1\texpanded\t1
runtime_state\tcontinuous\t1
supervision_state\tmode\tactive-preedit\tactive_preedit\t1
selected_candidate\t1\t我
visible_candidates\t0:我才\t1:我\t2:操
numbered_candidates\t1:0:我才\t2:1:我\t3:2:操
events\tletter:w\tletter:o\tcursor-move:Down\tspace:\tcandidate-selected:我
correction_events\tletter:s\tobserved:Tab\tletter:w\tletter:o\tcandidate-selected:我
context\tprior-secret-context\t更早
context_features\tv1:33333333333333333333333333333333\tv1:44444444444444444444444444444444
pending_segment\twoc\two\t我\tc
segment_chain\twoc\two\t我\tc\twocao\t我操
segment_chain\twoc\two\t我\tc\twoc\t我
preference\twoc\t我才\t3
preference\twoc\t我\t9
preference\twoc\t操\t18446744073709551615
correction\twoc\twocao\t2
"""
    raw_body = """protocol\t1
preedit\tgithub
application\tTerminal
candidates\tgithub
candidate_metadata\t0\tconsumed_prefix\t0\tsource\traw\tscore\t100
state\tpreedit_cursor\t6\tcandidate_cursor\t0\texpanded\t0
runtime_state\tcontinuous\t0
supervision_state\tmode\tactive-preedit\tactive_preedit\t1
selected_candidate\t0\tgithub
visible_candidates\t0:github
numbered_candidates\t1:0:github
events\tletter:g\tletter:i\tenter:\traw-committed:github
correction_events\tletter:g\tletter:i\tenter:\traw-committed:github
preference\tgithub\tgithub\t4
"""
    escape_body = """protocol\t1
preedit\tnihao
candidates\t你好\t你号
state\tpreedit_cursor\t5\tcandidate_cursor\t0\texpanded\t0
runtime_state\tcontinuous\t0
supervision_state\tmode\tactive-preedit\tactive_preedit\t1
selected_candidate\t0\t你好
visible_candidates\t0:你好\t1:你号
numbered_candidates\t1:0:你好\t2:1:你号
events\tletter:n\tletter:i\tescape:
correction_events\tletter:n\tletter:i\tescape:
"""
    intermediate_body = """protocol\t1
preedit\tni
application\tPrivateApp
candidates\t你\t呢
state\tpreedit_cursor\t2\tcandidate_cursor\t0\texpanded\t0
runtime_state\tcontinuous\t0
supervision_state\tmode\tactive-preedit\tactive_preedit\t1
selected_candidate\t0\t你
visible_candidates\t0:你\t1:呢
numbered_candidates\t1:0:你\t2:1:呢
events\tletter:n\tletter:i
correction_events\tletter:n\tletter:i
"""

    with tempfile.TemporaryDirectory(prefix="tipe-training-export-") as temporary:
        temporary_path = Path(temporary)
        history = temporary_path / "history.tsv"
        history.write_text(
            "---\tunix_ms\t1\tprogram\tSecretEditor\tpreedit\twoc\tcandidates\t3\texpanded\t1\n"
            + candidate_body
            + "---\tunix_ms\t2\tprogram\tSecretEditor\tpreedit\twoc\tcandidates\t3\texpanded\t1\n"
            + candidate_body
            + "---\tunix_ms\t3\tprogram\tTerminal\tpreedit\tgithub\tcandidates\t1\texpanded\t0\n"
            + raw_body
            + "---\tunix_ms\t4\tprogram\tEditor\tpreedit\tnihao\tcandidates\t2\texpanded\t0\n"
            + escape_body
            + "---\tunix_ms\t5\tprogram\tPrivateApp\tpreedit\tni\tcandidates\t2\texpanded\t0\n"
            + intermediate_body
            + "---\tunix_ms\t6\tprogram\tBroken\tpreedit\tx\tcandidates\t0\texpanded\t0\n"
            + "protocol\t2\npreedit\tx\ncandidates\n",
            encoding="utf-8",
        )

        result = run(exporter, history)
        samples = [json.loads(line) for line in result.stdout.splitlines() if line]
        require(len(samples) == 4, "default export should preserve repeated labeled behavior")
        require([sample["task"] for sample in samples] ==
                    ["candidate-selected", "candidate-selected", "raw-committed", "escape"],
                "default task ordering")
        candidate = samples[0]
        require(candidate["schema"] == "tipe.training.v1", "training schema")
        require(candidate["target"]["candidate_index"] == 1 and
                    candidate["target"]["consumed_prefix"] == 2 and
                    candidate["target"]["remaining_preedit"] == "c",
                "prefix target preserves remaining preedit")
        require(candidate["input"]["events"][-1] == {"type": "space", "text": ""},
                "terminal candidate event must not leak into model input")
        require("application" not in candidate["input"] and
                    "correction_events" not in candidate["input"] and
                    "context_features" not in candidate["input"] and
                    "surrounding_context_features" not in candidate["input"] and
                    "known_evidence" not in candidate["input"],
                "privacy-sensitive fields are opt-in")

        stats_result = run(exporter, history, "--stats")
        stats = json.loads(stats_result.stdout)
        require(stats["records"] == 6 and stats["samples"] == 4 and stats["malformed"] == 1 and
                    stats["skipped_unlabeled"] == 1 and stats["duplicates"] == 0 and
                    stats["feature_samples"] == {"context": 0, "surrounding": 0},
                "stats account for malformed, unlabeled, and duplicate records")
        context_stats = json.loads(run(
            exporter, history, "--stats", "--include-context", "--include-surrounding"
        ).stdout)
        require(context_stats["feature_samples"] == {"context": 2, "surrounding": 2},
                "stats report how many exported samples actually carry opaque context features")

        expanded_result = run(
            exporter,
            history,
            "--include-application",
            "--include-correction-trail",
            "--include-context",
            "--include-surrounding",
            "--include-evidence",
        )
        expanded = [json.loads(line) for line in expanded_result.stdout.splitlines() if line]
        require(len(expanded) == 4, "keep-duplicates should preserve repeated labeled records")
        first = expanded[0]
        require(first["input"]["application"] == "SecretEditor", "application opt-in")
        require(first["input"]["correction_events"][-1] == {"type": "letter", "text": "o"},
                "long correction trail excludes the target event")
        require(
            len(first["input"]["context_features"]) == 2
            and first["input"]["context_features"] == [
                "v1:33333333333333333333333333333333",
                "v1:44444444444444444444444444444444",
            ]
            and set(first["input"]["surrounding_context_features"]) == {"before", "after"}
            and first["input"]["surrounding_context_features"]["before"] ==
                "v1:11111111111111111111111111111111"
            and "prior-secret-context" not in expanded_result.stdout
            and "private-left" not in expanded_result.stdout
            and "private-right" not in expanded_result.stdout,
            "context opt-ins export stable fingerprints without retaining raw context text",
        )
        require(first["input"]["pending_segments"][0]["remaining_preedit"] == "c" and
                    len(first["input"]["segment_chains"]) == 1 and
                    first["input"]["segment_chains"][0]["combined_candidate"] == "我操" and
                    first["input"]["known_evidence"]["corrections"][0]["corrected_preedit"] == "wocao" and
                    first["input"]["known_evidence"]["preferences"] ==
                        [{"preedit": "woc", "candidate": "我才", "count": 3}],
                "evidence opt-in preserves prior rows but removes target-derived labels and invalid counts")
        require(
            expanded[2]["input"]["known_evidence"]["preferences"] == [],
            "raw-commit preference created by the target action is not exported as model input",
        )

        deduplicated_result = run(exporter, history, "--deduplicate")
        deduplicated = [json.loads(line) for line in deduplicated_result.stdout.splitlines() if line]
        require(len(deduplicated) == 3, "--deduplicate collapses equivalent samples")

        all_result = run(exporter, history, "--all", "--deduplicate")
        all_samples = [json.loads(line) for line in all_result.stdout.splitlines() if line]
        require(len(all_samples) == 4 and all_samples[-1]["task"] == "observation" and
                    all_samples[-1]["target"] is None,
                "--all includes unlabeled intermediate observations")

        cache_dir = temporary_path / "cache" / "tipe"
        cache_dir.mkdir(parents=True)
        (cache_dir / "supervision-history.tsv").write_text(
            "---\tunix_ms\t1\tpreedit\tni\n" + intermediate_body, encoding="utf-8"
        )
        (cache_dir / "supervision-training-history.tsv").write_text(
            "---\tunix_ms\t3\tpreedit\tgithub\n" + raw_body, encoding="utf-8"
        )
        default_result = subprocess.run(
            [sys.executable, str(exporter), "--stats"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "XDG_CACHE_HOME": str(temporary_path / "cache")},
            check=False,
        )
        require(default_result.returncode == 0 and
                    json.loads(default_result.stdout)["history"].endswith("supervision-training-history.tsv"),
                "default path prefers terminal training history")

        missing = run(exporter, temporary_path / "missing.tsv", expected=1)
        require("history file not found" in missing.stderr, "missing history diagnostic")

    print("training export ok")


if __name__ == "__main__":
    main()
