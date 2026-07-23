#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import sys
from collections import Counter
from pathlib import Path


SCHEMA = "tipe.training.v1"
TERMINAL_ACTIONS = {"candidate-selected", "raw-committed", "escape"}
MAX_LEARNING_COUNT = 1000000
MAX_CONTEXT_ITEMS = 16
MAX_CONTEXT_BYTES = 1024
CONTEXT_FINGERPRINT_PREFIX = "v1:"
CONTEXT_FINGERPRINT_HEX_LENGTH = 32
MASK64 = (1 << 64) - 1


def default_history_path():
    cache_home = os.environ.get("XDG_CACHE_HOME")
    if cache_home:
        cache_dir = Path(cache_home) / "tipe"
        training_history = cache_dir / "supervision-training-history.tsv"
        return training_history if training_history.is_file() else cache_dir / "supervision-history.tsv"
    home = os.environ.get("HOME")
    if not home:
        raise ValueError("HOME and XDG_CACHE_HOME are both unset")
    cache_dir = Path(home) / ".cache" / "tipe"
    training_history = cache_dir / "supervision-training-history.tsv"
    return training_history if training_history.is_file() else cache_dir / "supervision-history.tsv"


def unescape_field(text):
    result = []
    index = 0
    while index < len(text):
        if text[index] != "\\" or index + 1 >= len(text):
            result.append(text[index])
            index += 1
            continue
        escaped = text[index + 1]
        replacements = {"t": "\t", "r": "\r", "n": "\n", "\\": "\\"}
        if escaped in replacements:
            result.append(replacements[escaped])
            index += 2
        else:
            result.append("\\")
            index += 1
    return "".join(result)


def integer(text, default=0):
    try:
        return int(text)
    except (TypeError, ValueError):
        return default


def context_fingerprint_part(text, seed):
    value = seed
    for byte in b"TiPE-context-v1" + text.encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & MASK64
    value ^= value >> 33
    value = (value * 0xFF51AFD7ED558CCD) & MASK64
    value ^= value >> 33
    value = (value * 0xC4CEB9FE1A85EC53) & MASK64
    value ^= value >> 33
    return value


def context_fingerprint(text):
    encoded = text.encode("utf-8")
    if not text or len(encoded) > MAX_CONTEXT_BYTES:
        return None
    first = context_fingerprint_part(text, 14695981039346656037)
    second = context_fingerprint_part(text, 0x84222325CBF29CE4)
    return f"{CONTEXT_FINGERPRINT_PREFIX}{first:016x}{second:016x}"


def valid_context_fingerprint(value):
    if not isinstance(value, str) or not value.startswith(CONTEXT_FINGERPRINT_PREFIX):
        return False
    digest = value[len(CONTEXT_FINGERPRINT_PREFIX):]
    return len(digest) == CONTEXT_FINGERPRINT_HEX_LENGTH and all(
        character in "0123456789abcdef" for character in digest
    )


def key_values(fields, start=1):
    values = {}
    index = start
    while index + 1 < len(fields):
        values[fields[index]] = unescape_field(fields[index + 1])
        index += 2
    return values


def parse_events(fields):
    events = []
    for value in fields[1:]:
        kind, delimiter, text = value.partition(":")
        if not delimiter or not kind:
            continue
        events.append({"type": kind, "text": unescape_field(text)})
    return events


def parse_context_features(rows):
    features = []
    for fields in rows.get("context_features", []):
        for value in fields[1:]:
            if valid_context_fingerprint(value):
                features.append(value)
    if not features:
        for fields in rows.get("context", []):
            for value in fields[1:]:
                fingerprint = context_fingerprint(unescape_field(value))
                if fingerprint:
                    features.append(fingerprint)
    return features[-MAX_CONTEXT_ITEMS:]


def parse_surrounding_features(rows):
    features = {}
    for fields in rows.get("surrounding_features", []):
        for value in fields[1:]:
            side, delimiter, fingerprint = value.partition(":")
            if delimiter and side in {"before", "after"} and valid_context_fingerprint(fingerprint):
                features[side] = fingerprint
    for row_name, side in (("surrounding_before", "before"), ("surrounding_after", "after")):
        if side in features:
            continue
        fields_list = rows.get(row_name, [])
        if fields_list and len(fields_list[0]) > 1:
            fingerprint = context_fingerprint(unescape_field(fields_list[0][1]))
            if fingerprint:
                features[side] = fingerprint
    return features


def parse_history(path):
    records = []
    header = None
    body = []
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for raw_line in stream:
            line = raw_line.rstrip("\n")
            if line.endswith("\r"):
                line = line[:-1]
            if line.startswith("---\t"):
                if header is not None:
                    records.append((header, body))
                header = line
                body = []
            elif header is not None:
                body.append(line)
    if header is not None:
        records.append((header, body))
    return records


def row_map(lines):
    rows = {}
    for line in lines:
        fields = line.split("\t")
        if not fields or not fields[0]:
            continue
        rows.setdefault(fields[0], []).append(fields)
    return rows


def candidate_metadata(rows, candidate_count):
    metadata = [
        {"index": index, "consumed_prefix": 0, "source": "", "score": 0}
        for index in range(candidate_count)
    ]
    for fields in rows.get("candidate_metadata", []):
        if len(fields) < 2:
            continue
        index = integer(fields[1], -1)
        if index < 0 or index >= candidate_count:
            continue
        values = key_values(fields, 2)
        metadata[index] = {
            "index": index,
            "consumed_prefix": integer(values.get("consumed_prefix")),
            "source": values.get("source", ""),
            "score": integer(values.get("score")),
        }
    return metadata


def visible_indices(rows, candidate_count):
    result = []
    for fields in rows.get("visible_candidates", [])[:1]:
        for value in fields[1:]:
            index_text, delimiter, _ = value.partition(":")
            index = integer(index_text, -1)
            if delimiter and 0 <= index < candidate_count and index not in result:
                result.append(index)
    return result


def numbered_candidates(rows, candidate_count):
    result = []
    for fields in rows.get("numbered_candidates", [])[:1]:
        for value in fields[1:]:
            parts = value.split(":", 2)
            if len(parts) != 3:
                continue
            index = integer(parts[1], -1)
            if parts[0] and 0 <= index < candidate_count:
                result.append({"shortcut": parts[0], "index": index})
    return result


def selected_candidate(rows, candidates):
    fields_list = rows.get("selected_candidate", [])
    if not fields_list or len(fields_list[0]) < 3:
        return None
    fields = fields_list[0]
    index = integer(fields[1], -1)
    text = unescape_field(fields[2])
    if index < 0 or index >= len(candidates):
        return None
    return {"index": index, "text": text}


def parse_pending_segments(rows):
    result = []
    for fields in rows.get("pending_segment", []):
        if len(fields) < 5:
            continue
        result.append({
            "original_preedit": unescape_field(fields[1]),
            "consumed_preedit": unescape_field(fields[2]),
            "committed_text": unescape_field(fields[3]),
            "remaining_preedit": unescape_field(fields[4]),
        })
    return result


def parse_segment_chains(rows):
    result = []
    for fields in rows.get("segment_chain", []):
        if len(fields) < 7:
            continue
        result.append({
            "original_preedit": unescape_field(fields[1]),
            "consumed_preedit": unescape_field(fields[2]),
            "committed_text": unescape_field(fields[3]),
            "remaining_preedit": unescape_field(fields[4]),
            "corrected_full_preedit": unescape_field(fields[5]),
            "combined_candidate": unescape_field(fields[6]),
        })
    return result


def parse_evidence(rows):
    preferences = []
    for fields in rows.get("preference", []):
        if len(fields) >= 4:
            count = integer(fields[3])
            if count < 1 or count > MAX_LEARNING_COUNT:
                continue
            preferences.append({
                "preedit": unescape_field(fields[1]),
                "candidate": unescape_field(fields[2]),
                "count": count,
            })
    corrections = []
    for fields in rows.get("correction", []):
        if len(fields) >= 4:
            count = integer(fields[3])
            if count < 1 or count > MAX_LEARNING_COUNT:
                continue
            corrections.append({
                "typo": unescape_field(fields[1]),
                "corrected_preedit": unescape_field(fields[2]),
                "count": count,
            })
    return {"preferences": preferences, "corrections": corrections}


def terminal_action(events):
    if not events or events[-1]["type"] not in TERMINAL_ACTIONS:
        return None
    return events[-1]


def build_sample(header_line, lines, options):
    header_fields = header_line.split("\t")
    header = key_values(header_fields, 1)
    rows = row_map(lines)
    if rows.get("protocol", [["", ""]])[0][1:2] != ["1"]:
        raise ValueError("unsupported or missing protocol")
    preedit_rows = rows.get("preedit", [])
    candidate_rows = rows.get("candidates", [])
    if not preedit_rows or not candidate_rows:
        raise ValueError("missing preedit or candidates row")

    preedit = unescape_field(preedit_rows[0][1] if len(preedit_rows[0]) > 1 else "")
    candidates = [unescape_field(value) for value in candidate_rows[0][1:]]
    metadata = candidate_metadata(rows, len(candidates))
    state = key_values(rows.get("state", [["state"]])[0])
    runtime = key_values(rows.get("runtime_state", [["runtime_state"]])[0])
    supervision = key_values(rows.get("supervision_state", [["supervision_state"]])[0])
    events = parse_events(rows.get("events", [["events"]])[0])
    action = terminal_action(events)
    if action is None and not options.all_records:
        return None

    input_events = events[:-1] if action is not None else events
    long_events = []
    if options.include_correction_trail:
        long_events = parse_events(rows.get("correction_events", [["correction_events"]])[0])
        if action is not None and long_events and long_events[-1] == action:
            long_events = long_events[:-1]

    selected = selected_candidate(rows, candidates)
    target = None
    task = "observation"
    if action is not None:
        task = action["type"]
        target = {"action": action["type"], "text": action["text"]}
        if action["type"] == "candidate-selected":
            selected_index = selected["index"] if selected else -1
            if selected_index < 0 or selected_index >= len(candidates) or candidates[selected_index] != action["text"]:
                try:
                    selected_index = candidates.index(action["text"])
                except ValueError:
                    selected_index = -1
            target["candidate_index"] = selected_index if selected_index >= 0 else None
            consumed = metadata[selected_index]["consumed_prefix"] if selected_index >= 0 else 0
            target["consumed_prefix"] = consumed
            target["remaining_preedit"] = preedit[consumed:] if 0 < consumed < len(preedit) else ""

    sample = {
        "schema": SCHEMA,
        "record_id": hashlib.sha256((header_line + "\n" + "\n".join(lines)).encode("utf-8")).hexdigest()[:20],
        "timestamp_ms": integer(header.get("unix_ms")),
        "task": task,
        "input": {
            "preedit": preedit,
            "preedit_cursor": integer(state.get("preedit_cursor"), len(preedit)),
            "candidates": candidates,
            "candidate_metadata": metadata,
            "visible_candidate_indices": visible_indices(rows, len(candidates)),
            "numbered_candidates": numbered_candidates(rows, len(candidates)),
            "expanded": state.get("expanded", header.get("expanded", "0")) == "1",
            "continuous_mode": runtime.get("continuous", "0") == "1",
            "supervision_mode": supervision.get("mode", "active-preedit" if preedit else "pass-through-only"),
            "events": input_events,
        },
        "ui_at_action": {
            "candidate_cursor": integer(state.get("candidate_cursor")),
            "selected_candidate": selected,
        },
        "target": target,
    }
    if options.include_application:
        application_rows = rows.get("application", [])
        sample["input"]["application"] = (
            unescape_field(application_rows[0][1])
            if application_rows and len(application_rows[0]) > 1
            else header.get("program", "")
        )
    if options.include_correction_trail:
        sample["input"]["correction_events"] = long_events
    if options.include_context:
        sample["input"]["context_features"] = parse_context_features(rows)
    if options.include_surrounding:
        sample["input"]["surrounding_context_features"] = parse_surrounding_features(rows)
    if options.include_evidence:
        sample["input"]["pending_segments"] = parse_pending_segments(rows)
        segment_chains = parse_segment_chains(rows)
        evidence = parse_evidence(rows)
        if target is not None:
            target_text = target.get("text")
            evidence["preferences"] = [
                preference for preference in evidence["preferences"]
                if not (
                    preference.get("preedit") == preedit
                    and preference.get("candidate") == target_text
                )
            ]
            segment_chains = [
                chain for chain in segment_chains
                if not (
                    chain.get("combined_candidate") == target_text
                    and preedit in {chain.get("original_preedit"), chain.get("corrected_full_preedit")}
                )
            ]
        sample["input"]["segment_chains"] = segment_chains
        sample["input"]["known_evidence"] = evidence
    return sample


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Export bounded TiPE supervision history as privacy-controlled JSONL training samples."
    )
    parser.add_argument("--history", type=Path, help="supervision-history.tsv path")
    parser.add_argument("--all", dest="all_records", action="store_true", help="include unlabeled intermediate records")
    parser.add_argument("--include-application", action="store_true", help="include focused application names")
    parser.add_argument(
        "--include-correction-trail", action="store_true", help="include the longer cross-composition key trail"
    )
    parser.add_argument(
        "--include-context", action="store_true",
        help="include bounded recent-commit fingerprints without retaining committed text",
    )
    parser.add_argument(
        "--include-surrounding", action="store_true",
        help="include bounded before/after-cursor fingerprints without retaining surrounding text",
    )
    parser.add_argument("--include-evidence", action="store_true", help="include known preference/correction/segment evidence")
    parser.add_argument("--deduplicate", action="store_true", help="collapse equivalent samples instead of preserving repetition")
    parser.add_argument("--limit", type=int, default=0, help="emit only the latest N samples")
    parser.add_argument("--stats", action="store_true", help="print export statistics as one JSON object")
    options = parser.parse_args(argv)
    if options.limit < 0:
        parser.error("--limit must be zero or greater")
    return options


def main(argv=None):
    options = parse_args(argv)
    try:
        history_path = options.history or default_history_path()
    except ValueError as error:
        print(f"tipe-training-export: {error}", file=sys.stderr)
        return 2
    if not history_path.is_file():
        print(f"tipe-training-export: history file not found: {history_path}", file=sys.stderr)
        return 1

    records = parse_history(history_path)
    samples = []
    malformed = 0
    skipped_unlabeled = 0
    duplicates = 0
    seen = set()
    task_counts = Counter()
    for header, lines in records:
        try:
            sample = build_sample(header, lines, options)
        except (IndexError, ValueError):
            malformed += 1
            continue
        if sample is None:
            skipped_unlabeled += 1
            continue
        signature_value = dict(sample)
        signature_value.pop("record_id", None)
        signature_value.pop("timestamp_ms", None)
        signature = json.dumps(signature_value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        if options.deduplicate and signature in seen:
            duplicates += 1
            continue
        seen.add(signature)
        task_counts[sample["task"]] += 1
        samples.append(sample)

    if options.limit:
        samples = samples[-options.limit:]
        task_counts = Counter(sample["task"] for sample in samples)

    if options.stats:
        context_feature_samples = sum(bool(sample["input"].get("context_features")) for sample in samples)
        surrounding_feature_samples = sum(
            bool(sample["input"].get("surrounding_context_features")) for sample in samples
        )
        stats = {
            "schema": "tipe.training-export.stats.v1",
            "history": str(history_path),
            "records": len(records),
            "samples": len(samples),
            "malformed": malformed,
            "skipped_unlabeled": skipped_unlabeled,
            "duplicates": duplicates,
            "tasks": dict(sorted(task_counts.items())),
            "feature_samples": {
                "context": context_feature_samples,
                "surrounding": surrounding_feature_samples,
            },
            "includes": {
                "application": options.include_application,
                "correction_trail": options.include_correction_trail,
                "context": options.include_context,
                "surrounding": options.include_surrounding,
                "evidence": options.include_evidence,
            },
        }
        print(json.dumps(stats, ensure_ascii=False, sort_keys=True))
    else:
        for sample in samples:
            print(json.dumps(sample, ensure_ascii=False, sort_keys=True, separators=(",", ":")))

    if malformed:
        print(f"tipe-training-export: skipped {malformed} malformed record(s)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
