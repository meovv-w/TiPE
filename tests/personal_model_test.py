#!/usr/bin/env python3

import json
import hashlib
import fcntl
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(command, *, stdin="", expected=0, env=None):
    result = subprocess.run(
        command,
        input=stdin,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        check=False,
    )
    require(result.returncode == expected, f"unexpected exit {result.returncode}: {result.stderr}")
    return result


def signed_feature_weight(name, dimension, value):
    digest = hashlib.blake2b(name.encode("utf-8"), digest_size=9, person=b"TiPErank").digest()
    index = int.from_bytes(digest[:8], "little") % dimension
    sign = 1.0 if digest[8] & 1 else -1.0
    return str(index), value * sign


def correction_trail(typo, corrected):
    return (
        [{"type": "letter", "text": character} for character in typo]
        + [{"type": "backspace", "text": ""} for _ in typo]
        + [{"type": "letter", "text": character} for character in corrected]
    )


def sample(
    record_id, preedit, candidates, sources, selected, *, correction=False, correction_from=None,
    consumed_prefix=0,
):
    events = [
        {"type": "letter", "text": preedit[0]},
        {"type": "space", "text": ""},
    ]
    observation = {
        "preedit": preedit,
        "preedit_cursor": len(preedit),
        "candidates": candidates,
        "candidate_metadata": [
            {
                "index": index,
                "consumed_prefix": consumed_prefix if index == selected else 0,
                "source": source,
                "score": 100 - index,
            }
            for index, source in enumerate(sources)
        ],
        "events": events,
        "expanded": False,
        "continuous_mode": False,
        "supervision_mode": "active-preedit",
    }
    if correction_from:
        observation["correction_events"] = correction_trail(correction_from, preedit)
    elif correction:
        observation["correction_events"] = [
            {"type": "letter", "text": "x"},
            {"type": "backspace", "text": ""},
            {"type": "letter", "text": preedit[0]},
        ]
    return {
        "schema": "tipe.training.v1",
        "record_id": record_id,
        "timestamp_ms": int(record_id),
        "task": "candidate-selected",
        "input": observation,
        "ui_at_action": {"candidate_cursor": selected},
        "target": {
            "action": "candidate-selected",
            "text": candidates[selected],
            "candidate_index": selected,
            "consumed_prefix": consumed_prefix,
            "remaining_preedit": preedit[consumed_prefix:] if consumed_prefix else "",
        },
    }


def raw_sample(record_id, preedit, candidates, sources):
    observation = {
        "preedit": preedit,
        "preedit_cursor": len(preedit),
        "candidates": candidates,
        "candidate_metadata": [
            {"index": index, "consumed_prefix": 0, "source": source, "score": 100 - index}
            for index, source in enumerate(sources)
        ],
        "events": [{"type": "letter", "text": preedit[0]}, {"type": "raw-committed", "text": preedit}],
        "expanded": False,
        "continuous_mode": False,
        "supervision_mode": "active-preedit",
    }
    return {
        "schema": "tipe.training.v1",
        "record_id": record_id,
        "timestamp_ms": int(record_id),
        "task": "raw-committed",
        "input": observation,
        "ui_at_action": {"candidate_cursor": 0, "selected_candidate": None},
        "target": {"action": "raw-committed", "text": preedit},
    }


def request(preedit, candidates, sources, *, correction=False):
    lines = [
        "protocol\t1",
        f"preedit\t{preedit}",
        "candidates\t" + "\t".join(candidates),
    ]
    for index, source in enumerate(sources):
        lines.append(
            f"candidate_metadata\t{index}\tconsumed_prefix\t0\tsource\t{source}\tscore\t{100 - index}"
        )
    lines.append(f"events\tletter:{preedit[0]}\tspace:")
    if correction:
        lines.append(f"correction_events\tletter:x\tbackspace:\tletter:{preedit[0]}")
    return "\n".join(lines) + "\n"


def history_record(training_sample):
    observation = training_sample["input"]
    target = training_sample["target"]
    candidates = observation["candidates"]
    selected = target["candidate_index"]
    lines = [
        f"---\tunix_ms\t{training_sample['timestamp_ms']}\tprogram\tTest\tpreedit\t{observation['preedit']}"
        f"\tcandidates\t{len(candidates)}\texpanded\t0\tterminal\t1",
        "protocol\t1",
        f"preedit\t{observation['preedit']}",
        "candidates\t" + "\t".join(candidates),
    ]
    for metadata in observation["candidate_metadata"]:
        lines.append(
            f"candidate_metadata\t{metadata['index']}\tconsumed_prefix\t{metadata['consumed_prefix']}"
            f"\tsource\t{metadata['source']}\tscore\t{metadata['score']}"
        )
    lines.extend([
        f"state\tpreedit_cursor\t{len(observation['preedit'])}\tcandidate_cursor\t{selected}\texpanded\t0",
        "runtime_state\tcontinuous\t0",
        "supervision_state\tmode\tactive-preedit\tactive_preedit\t1",
        f"selected_candidate\t{selected}\t{target['text']}",
        "visible_candidates\t" + "\t".join(f"{index}:{text}" for index, text in enumerate(candidates)),
        "numbered_candidates\t" +
        "\t".join(f"{index + 1}:{index}:{text}" for index, text in enumerate(candidates)),
        "events\t" + "\t".join(
            [f"{event['type']}:{event['text']}" for event in observation["events"]] +
            [f"candidate-selected:{target['text']}"]
        ),
    ])
    correction_events = observation.get("correction_events", observation["events"])
    lines.append(
        "correction_events\t" + "\t".join(
            [f"{event['type']}:{event['text']}" for event in correction_events] +
            [f"candidate-selected:{target['text']}"]
        )
    )
    for correction in observation.get("known_evidence", {}).get("corrections", []):
        lines.append(
            f"correction\t{correction['typo']}\t{correction['corrected_preedit']}\t{correction['count']}"
        )
    return "\n".join(lines) + "\n"


def main():
    require(len(sys.argv) == 3, "expected personal model and trainer paths")
    personal_model = Path(sys.argv[1])
    personal_trainer = Path(sys.argv[2])
    require(personal_model.is_file(), f"missing personal model: {personal_model}")
    require(personal_trainer.is_file(), f"missing personal trainer: {personal_trainer}")

    samples = [
        sample("1", "start", ["开始", "start"], ["lookup", "raw-offer"], 1),
        sample("2", "start", ["开始", "start"], ["lookup", "raw-offer"], 1),
        sample("3", "ihao", ["一号", "你好"], ["lookup", "correction"], 1, correction=True),
        sample("4", "engli", ["恩格里", "能力"], ["lookup", "local-correction"], 1, correction=True),
        sample("5", "zhongguo", ["中国", "中"], ["lookup", "prefix"], 0),
        sample("6", "shijie", ["世界", "是"], ["lookup", "prefix"], 0),
        sample("7", "nihao", ["拟好", "你好"], ["lookup", "lookup"], 1, correction_from="ihao"),
        sample("8", "nengli", ["能里", "能力"], ["lookup", "lookup"], 1, correction_from="engli"),
        sample(
            "11", "qingzaisaoyici", ["轻载扫一次", "清", "请"], ["lookup", "prefix", "prefix"], 2,
            consumed_prefix=4,
        ),
        sample("9", "start", ["开始", "start"], ["lookup", "raw-offer"], 1),
        sample("10", "zhongguo", ["中国", "中"], ["lookup", "prefix"], 0),
        sample("12", "women", ["我们", "窝们"], ["lookup", "lookup"], 0),
        sample("13", "tianqi", ["天气", "天启"], ["lookup", "lookup"], 0),
    ]
    samples[8]["input"]["candidate_metadata"][1]["consumed_prefix"] = 4
    samples[0]["input"]["known_evidence"] = {
        "preferences": [],
        "corrections": [
            {"typo": "niyhao", "corrected_preedit": "nihao", "count": 3},
            {"typo": "gog", "corrected_preedit": "gong", "count": 1},
        ],
    }

    with tempfile.TemporaryDirectory(prefix="tipe-personal-model-") as temporary:
        temporary_path = Path(temporary)
        training = temporary_path / "training.jsonl"
        training.write_text("".join(json.dumps(item, ensure_ascii=False) + "\n" for item in samples), encoding="utf-8")
        pinyin_dictionary = temporary_path / "pinyin-test.dict.yaml"
        pinyin_dictionary.write_text(
            "---\nname: tipe_test\n...\n"
            "你好\tni hao\t2048\n"
            "你们\tni men\t1024\n"
            "拟人\tni ren\t1024\n"
            "上\tshang\t2048\n"
            "配合\tpei he\t8192\n"
            "测试丙\tpie he\t1\n"
            "测试甲\tlueinu\t8\n"
            "测试乙\tlueniu\t1024\n",
            encoding="utf-8",
        )
        model = temporary_path / "model.json"
        model_again = temporary_path / "model-again.json"
        train_args = [
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(training),
            "--dimension",
            "4096",
            "--epochs",
            "16",
            "--seed",
            "9",
            "--min-samples",
            "4",
            "--pinyin-dictionary",
            str(pinyin_dictionary),
        ]
        trained = run(train_args + ["--output", str(model)])
        require(
            "name\tTiP" in trained.stdout
            and "samples\t13" in trained.stdout
            and "skipped\t0" in trained.stdout
            and "accuracy\t13/13" in trained.stdout
            and "active-correction-patterns\t2" in trained.stdout
            and "active-key-habits\t0" in trained.stdout
            and "feature-version\t4" in trained.stdout
            and "pinyin-prior-entries\t8" in trained.stdout
            and "pinyin-prior-sources\t1" in trained.stdout
            and "chinese-ranking-samples\t10" in trained.stdout
            and "non-leading-samples\t5" in trained.stdout
            and "validation-accuracy\t" in trained.stdout
            and "validation-baseline-accuracy\t" in trained.stdout
            and "validation-gain\t" in trained.stdout
            and "validation-non-leading-accuracy\t" in trained.stdout
            and "validation-strategy\tcapability-isolated-temporal-v4" in trained.stdout
            and "validation-generic-non-leading-accuracy\t" in trained.stdout
            and "recommendation\t" in trained.stdout,
                "trainer reports complete fit")
        run(train_args + ["--output", str(model_again)])
        require(model.read_bytes() == model_again.read_bytes(), "training is deterministic for a fixed seed")

        model_text = model.read_text(encoding="utf-8")
        require(
            "start" not in model_text
            and "开始" not in model_text
            and "你好" not in model_text
            and "letter:" not in model_text,
            "hashed model does not retain source text or the raw key sequence",
        )
        model_data = json.loads(model_text)
        require(
            model_data["schema"] == "tipe.personal-reranker.v1"
            and model_data["name"] == "TiP"
            and model_data["architecture"] ==
                "hashed-pairwise-ranker+personal-edit-channel+raw-token-memory+"
                "raw-offer-profile+pinyin-prior"
            and model_data["feature_version"] == 4
            and model_data["raw_token_evidence"] == {}
            and model_data["training"]["samples"] == 13
            and model_data["training"]["chinese_ranking_samples"] == 10
            and model_data["pinyin_prior"] == {
                "lueinu": 4,
                "lueniu": 11,
                "nihao": 12,
                "nimen": 11,
                "niren": 11,
                "peihe": 14,
                "piehe": 1,
                "shang": 12,
            }
            and model_data["promotion_margin"] == 0.5
            and model_data["training"]["validation_samples"] == 2
            and model_data["training"]["validation_strategy"] == "capability-isolated-temporal-v4"
            and "validation_generic_non_leading_samples" in model_data["training"]
            and model_data["training"]["validation_gain"] >= 0
            and model_data["training"]["non_leading_samples"] == 5
            and model_data["training"]["validation_non_leading_samples"] > 0
            and model_data["training"]["recommendation"] in {"ready", "collect-more-data"}
            and model_data["correction_patterns"] == [
                {
                    "count": 3,
                    "kind": "extra",
                    "position": 2,
                    "relative_to_end": False,
                    "replacement": "",
                    "typed": "y",
                },
                {
                    "count": 2,
                    "kind": "missing",
                    "position": 0,
                    "relative_to_end": False,
                    "replacement": "n",
                    "typed": "",
                },
                {
                    "count": 1,
                    "kind": "missing",
                    "position": 1,
                    "relative_to_end": True,
                    "replacement": "n",
                    "typed": "",
                },
            ]
            and model_data["key_habits"] == [
                {"count": 3, "kind": "extra", "replacement": "", "typed": "y"},
                {"count": 3, "kind": "missing", "replacement": "n", "typed": ""},
            ],
                "model schema and metadata")

        inspected = run([sys.executable, str(personal_model), "inspect", "--model", str(model)])
        require(
            "name\tTiP" in inspected.stdout
            and "schema\ttipe.personal-reranker.v1" in inspected.stdout
            and "architecture\thashed-pairwise-ranker+personal-edit-channel+raw-token-memory+"
                "raw-offer-profile+pinyin-prior" in
                inspected.stdout
            and "feature-version\t4" in inspected.stdout
            and "pinyin-prior-entries\t8" in inspected.stdout
            and "training-samples\t13" in inspected.stdout
            and "training-chinese-ranking-samples\t10" in inspected.stdout
            and "active-correction-patterns\t2" in inspected.stdout
            and "active-key-habits\t0" in inspected.stdout
            and "raw-token-evidence\t0" in inspected.stdout
            and "active-raw-token-evidence\t0" in inspected.stdout
            and "promotion-margin\t0.5" in inspected.stdout
            and "training-validation-samples\t2" in inspected.stdout
            and "training-validation-strategy\tcapability-isolated-temporal-v4" in inspected.stdout
            and "training-validation-generic-non-leading-samples\t" in inspected.stdout
            and "training-validation-gain\t" in inspected.stdout
            and "training-non-leading-samples\t5" in inspected.stdout
            and "training-validation-non-leading-samples\t" in inspected.stdout
            and f"training-recommendation\t{model_data['training']['recommendation']}" in inspected.stdout,
                "model inspect output")

        all_leading_training = temporary_path / "all-leading.jsonl"
        all_leading_samples = [
            sample(
                str(100 + index),
                f"nihao{index}",
                [f"你好{index}", f"你号{index}"],
                ["lookup", "lookup"],
                0,
            )
            for index in range(10)
        ]
        all_leading_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in all_leading_samples),
            encoding="utf-8",
        )
        all_leading_model = temporary_path / "all-leading-model.json"
        all_leading = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(all_leading_training),
            "--output",
            str(all_leading_model),
            "--min-samples",
            "4",
        ])
        all_leading_data = json.loads(all_leading_model.read_text(encoding="utf-8"))
        require(
            "validation-gain\t0" in all_leading.stdout
            and "validation-non-leading-accuracy\t0/0" in all_leading.stdout
            and "recommendation\tcollect-more-data" in all_leading.stdout
            and all_leading_data["training"]["recommendation"] == "collect-more-data",
            "a model that only ties the first-candidate baseline is not ready",
        )

        stratified_training = temporary_path / "stratified-training.jsonl"
        stratified_samples = [
            sample(
                str(150 + index),
                f"toolx{index}",
                [f"工具{index}", f"自定义工具{index}"],
                ["lookup", "user"],
                1,
            )
            for index in range(10)
        ] + [
            sample(
                str(170 + index),
                f"normalx{index}",
                [f"正常{index}", f"常规{index}"],
                ["lookup", "lookup"],
                0,
            )
            for index in range(20)
        ]
        stratified_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in stratified_samples),
            encoding="utf-8",
        )
        stratified_model = temporary_path / "stratified-model.json"
        stratified = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(stratified_training),
            "--output",
            str(stratified_model),
            "--dimension",
            "4096",
            "--epochs",
            "16",
            "--min-samples",
            "4",
            "--no-pinyin-prior",
        ])
        stratified_data = json.loads(stratified_model.read_text(encoding="utf-8"))
        require(
            "validation-non-leading-accuracy\t5/5" in stratified.stdout
            and "validation-generic-non-leading-accuracy\t5/5" in stratified.stdout
            and stratified_data["training"]["validation_samples"] == 6
            and stratified_data["training"]["validation_non_leading_samples"] == 5
            and stratified_data["training"]["validation_generic_non_leading_samples"] == 5
            and stratified_data["training"]["validation_generic_non_leading_correct"] == 5
            and stratified_data["training"]["validation_leading_samples"] == 1
            and stratified_data["training"]["recommendation"] == "ready"
            and stratified_data["training"]["generic_ranking_safe"] is True,
            "temporal validation reserves enough older non-leading choices to test generic ranking safely",
        )

        evidence_only_samples = json.loads(json.dumps(stratified_samples, ensure_ascii=False))
        for item in evidence_only_samples[:10]:
            observation = item["input"]
            selected = item["target"]["candidate_index"]
            observation["known_evidence"] = {
                "preferences": [{
                    "preedit": observation["preedit"],
                    "candidate": observation["candidates"][selected],
                    "count": 4,
                }],
                "corrections": [],
            }
        evidence_only_training = temporary_path / "evidence-only-training.jsonl"
        evidence_only_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in evidence_only_samples),
            encoding="utf-8",
        )
        evidence_only_model = temporary_path / "evidence-only-model.json"
        evidence_only = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(evidence_only_training),
            "--output",
            str(evidence_only_model),
            "--dimension",
            "4096",
            "--epochs",
            "16",
            "--min-samples",
            "4",
            "--no-pinyin-prior",
        ])
        evidence_only_data = json.loads(evidence_only_model.read_text(encoding="utf-8"))
        require(
            "validation-non-leading-accuracy\t5/5" in evidence_only.stdout
            and "validation-generic-non-leading-accuracy\t0/0" in evidence_only.stdout
            and evidence_only_data["training"]["validation_generic_excluded_direct_evidence"] == 5
            and evidence_only_data["training"]["generic_ranking_safe"] is False,
            "direct preference evidence cannot masquerade as unseen generic ranking",
        )

        seen_preedit_samples = [
            sample(
                str(500 + index),
                "repeatx",
                ["重复", "自定义重复"],
                ["lookup", "user"],
                1,
            )
            for index in range(10)
        ] + stratified_samples[10:]
        seen_preedit_training = temporary_path / "seen-preedit-training.jsonl"
        seen_preedit_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in seen_preedit_samples),
            encoding="utf-8",
        )
        seen_preedit_model = temporary_path / "seen-preedit-model.json"
        seen_preedit = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(seen_preedit_training),
            "--output",
            str(seen_preedit_model),
            "--dimension",
            "4096",
            "--epochs",
            "16",
            "--min-samples",
            "4",
            "--no-pinyin-prior",
        ])
        seen_preedit_data = json.loads(seen_preedit_model.read_text(encoding="utf-8"))
        require(
            "validation-non-leading-accuracy\t5/5" in seen_preedit.stdout
            and "validation-generic-non-leading-accuracy\t0/0" in seen_preedit.stdout
            and seen_preedit_data["training"]["validation_generic_excluded_seen_preedit"] == 5
            and seen_preedit_data["training"]["generic_ranking_safe"] is False,
            "memorizing a repeated preedit cannot masquerade as unseen generic ranking",
        )

        derived_prefix_samples = json.loads(json.dumps(stratified_samples, ensure_ascii=False))
        for item in derived_prefix_samples[:10]:
            item["input"]["derived_prefix_selection"] = True
        derived_prefix_training = temporary_path / "derived-prefix-training.jsonl"
        derived_prefix_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in derived_prefix_samples),
            encoding="utf-8",
        )
        derived_prefix_model = temporary_path / "derived-prefix-model.json"
        derived_prefix = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(derived_prefix_training),
            "--output",
            str(derived_prefix_model),
            "--dimension",
            "4096",
            "--epochs",
            "16",
            "--min-samples",
            "4",
            "--no-pinyin-prior",
        ])
        derived_prefix_data = json.loads(derived_prefix_model.read_text(encoding="utf-8"))
        require(
            "validation-generic-non-leading-accuracy\t0/0" in derived_prefix.stdout
            and "derived-prefix:5" in derived_prefix.stdout
            and derived_prefix_data["training"]["validation_non_leading_samples"] == 5
            and derived_prefix_data["training"]["validation_generic_excluded_derived_prefix"] == 5
            and derived_prefix_data["training"]["generic_ranking_safe"] is False,
            "exact-only derived prefix choices cannot masquerade as unseen generic ranking\n"
            + derived_prefix.stdout,
        )

        raw_training = temporary_path / "raw-training.jsonl"
        raw_samples = [
            raw_sample(str(200 + index), "start", ["开始", "start"], ["lookup", "raw-offer"])
            for index in range(4)
        ]
        raw_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in raw_samples),
            encoding="utf-8",
        )
        raw_model = temporary_path / "raw-model.json"
        raw_trained = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(raw_training),
            "--output",
            str(raw_model),
            "--min-samples",
            "4",
            "--validation-percent",
            "0",
        ])
        raw_ranked = run(
            [sys.executable, str(personal_model), "predict", "--model", str(raw_model)],
            stdin=request("start", ["开始", "start"], ["lookup", "raw-offer"]),
        )
        require(
            "samples\t4" in raw_trained.stdout
            and "chinese-ranking-samples\t0" in raw_trained.stdout
            and "non-leading-samples\t0" in raw_trained.stdout
            and "raw-profile-accepted\t4" in raw_trained.stdout
            and "raw-profile-rejected\t0" in raw_trained.stdout
            and "raw-profile-safe\t0" in raw_trained.stdout
            and raw_ranked.stdout.splitlines()[0] == "candidate\tstart",
            "repeated exact raw commits remain active even before the aggregate raw profile is safe",
        )

        raw_profile_training = temporary_path / "raw-profile-training.jsonl"
        raw_profile_samples = []
        for index in range(6):
            raw_profile_samples.append(
                raw_sample(
                    str(220 + index), f"tool{index}", [f"工具{index}", f"tool{index}"],
                    ["lookup", "raw-offer"],
                )
            )
        for index in range(6):
            raw_profile_samples.append(
                sample(
                    str(230 + index), f"plainx{index}", [f"常用{index}", f"plainx{index}"],
                    ["lookup", "raw-offer"], 0,
                )
            )
        for index in range(4):
            auxiliary = raw_sample(
                str(240 + index), f"toolaux{index}", [f"toolaux{index}"], ["raw-pass-through"]
            )
            auxiliary["input"]["supervision_mode"] = "pass-through-only"
            raw_profile_samples.append(auxiliary)
        raw_profile_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in raw_profile_samples),
            encoding="utf-8",
        )
        raw_profile_model = temporary_path / "raw-profile-model.json"
        raw_profile_trained = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(raw_profile_training),
            "--output",
            str(raw_profile_model),
            "--dimension",
            "4096",
            "--epochs",
            "24",
            "--min-samples",
            "4",
            "--no-pinyin-prior",
        ])
        raw_profile_unseen_accepted = run(
            [sys.executable, str(personal_model), "predict", "--model", str(raw_profile_model), "--explain"],
            stdin=request("tool9", ["工具九", "tool9"], ["lookup", "raw-offer"]),
        )
        raw_profile_unseen_rejected = run(
            [sys.executable, str(personal_model), "predict", "--model", str(raw_profile_model), "--explain"],
            stdin=request("plainx9", ["常用九", "plainx9"], ["lookup", "raw-offer"]),
        )
        require(
            "raw-profile-validation-accuracy\t4/4" in raw_profile_trained.stdout
            and "raw-profile-validation-false-promotions\t0" in raw_profile_trained.stdout
            and "raw-profile-auxiliary-positive\t4" in raw_profile_trained.stdout
            and "raw-profile-safe\t1" in raw_profile_trained.stdout
            and "generic-ranking-safe\t0" in raw_profile_trained.stdout
            and raw_profile_unseen_accepted.stdout.splitlines()[0] == "candidate\ttool9"
            and "support\tvalidated-raw-profile" in raw_profile_unseen_accepted.stderr
            and raw_profile_unseen_rejected.stdout.splitlines()[0] == "candidate\t常用九",
            "raw-offer generalization has an independent balanced holdout with zero false promotions\n"
            + raw_profile_trained.stdout
            + raw_profile_unseen_accepted.stderr
            + raw_profile_unseen_rejected.stderr,
        )

        pass_through_training = temporary_path / "pass-through-correction-training.jsonl"
        pass_through_samples = []
        for index in range(5):
            item = raw_sample(str(250 + index), "nihao", ["nihao"], ["raw-pass-through"])
            item["input"]["supervision_mode"] = "pass-through-only"
            item["input"]["correction_events"] = correction_trail("ihao", "nihao")
            pass_through_samples.append(item)
        pass_through_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in pass_through_samples),
            encoding="utf-8",
        )
        pass_through_model = temporary_path / "pass-through-correction-model.json"
        pass_through_trained = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(pass_through_training),
            "--output",
            str(pass_through_model),
            "--min-samples",
            "5",
            "--validation-percent",
            "0",
        ])
        pass_through_generalized = run(
            [sys.executable, str(personal_model), "predict", "--model", str(pass_through_model)],
            stdin=request("imen", ["一门", "疑闷"], ["lookup", "lookup"]),
        )
        pass_through_exact_raw = run(
            [
                sys.executable,
                str(personal_model),
                "predict",
                "--model",
                str(pass_through_model),
                "--explain",
            ],
            stdin=request("nihao", ["你好", "nihao"], ["lookup", "raw-offer"]),
        )
        pass_through_model_data = json.loads(pass_through_model.read_text(encoding="utf-8"))
        require(
            "samples\t5" in pass_through_trained.stdout
            and "ranking-samples\t0" in pass_through_trained.stdout
            and "correction-only-samples\t5" in pass_through_trained.stdout
            and "raw-profile-auxiliary-positive\t5" in pass_through_trained.stdout
            and "raw-profile-safe\t0" in pass_through_trained.stdout
            and "raw-token-evidence\t1" in pass_through_trained.stdout
            and "active-raw-token-evidence\t1" in pass_through_trained.stdout
            and "skipped\t0" in pass_through_trained.stdout
            and "active-key-habits\t1" in pass_through_trained.stdout
            and pass_through_model_data["training"]["raw_profile_auxiliary_positive_samples"] == 5
            and len(pass_through_model_data["raw_profile_weights"]) > 0
            and len(pass_through_model_data["raw_token_evidence"]) == 1
            and "correction\timen\tnimen" in pass_through_generalized.stdout.splitlines()
            and pass_through_exact_raw.stdout.splitlines()[0] == "candidate\tnihao"
            and "support\trepeated-english-token" in pass_through_exact_raw.stderr,
            "English pass-through samples train correction and exact raw-token memory independently",
        )

        raw_override_training = temporary_path / "raw-token-override-training.jsonl"
        raw_override_samples = pass_through_samples + [
            sample("270", "nihao", ["你好", "nihao"], ["lookup", "raw-offer"], 0),
            sample("271", "nihao", ["你好", "nihao"], ["lookup", "raw-offer"], 0),
        ]
        raw_override_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in raw_override_samples),
            encoding="utf-8",
        )
        raw_override_model = temporary_path / "raw-token-override-model.json"
        run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(raw_override_training),
            "--output",
            str(raw_override_model),
            "--min-samples",
            "5",
            "--validation-percent",
            "0",
        ])
        raw_override_prediction = run(
            [
                sys.executable,
                str(personal_model),
                "predict",
                "--model",
                str(raw_override_model),
                "--explain",
            ],
            stdin=request("nihao", ["你好", "nihao"], ["lookup", "raw-offer"]),
        )
        require(
            raw_override_prediction.stdout.splitlines()[0] == "candidate\t你好"
            and "support\trepeated-english-token" not in raw_override_prediction.stderr,
            "repeated explicit Chinese choices override passive English token frequency",
        )

        runtime_training = temporary_path / "runtime-raw-training.jsonl"
        runtime_samples = list(pass_through_samples)
        for index in range(3):
            item = raw_sample(str(280 + index), "foobar", ["foobar"], ["raw-pass-through"])
            item["input"]["supervision_mode"] = "pass-through-only"
            runtime_samples.append(item)
        runtime_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in runtime_samples),
            encoding="utf-8",
        )
        runtime_preferences = temporary_path / "runtime-preferences.tsv"
        runtime_preferences.write_text(
            "foobar\tfoobar\t1\nnihao\t你好\t4\n__correction__\tihao\tnihao\t2\n",
            encoding="utf-8",
        )
        distilled = run([
            sys.executable,
            str(personal_model),
            "distill-raw",
            "--input",
            str(runtime_training),
            "--preferences",
            str(runtime_preferences),
        ])
        distilled_again = run([
            sys.executable,
            str(personal_model),
            "distill-raw",
            "--input",
            str(runtime_training),
            "--preferences",
            str(runtime_preferences),
        ])
        runtime_preference_text = runtime_preferences.read_text(encoding="utf-8")
        require(
            "runtime-raw-token-candidates\t1" in distilled.stdout
            and "runtime-raw-token-active\t1" in distilled.stdout
            and "runtime-raw-token-updated\t1" in distilled.stdout
            and "runtime-raw-token-updated\t0" in distilled_again.stdout
            and "__raw_token__\tfoobar\t3\n" in runtime_preference_text
            and "foobar\tfoobar\t" not in runtime_preference_text
            and "nihao\tnihao\t" not in runtime_preference_text
            and "nihao\t你好\t4\n" in runtime_preference_text
            and "__correction__\tihao\tnihao\t2\n" in runtime_preference_text
            and runtime_preferences.stat().st_mode & 0o777 == 0o600,
            "clicked TiP training idempotently distills safe exact English memory for normal runtime ranking",
        )
        runtime_distilled = run([
            sys.executable,
            str(personal_model),
            "distill-runtime",
            "--input",
            str(runtime_training),
            "--model",
            str(pass_through_model),
            "--preferences",
            str(runtime_preferences),
        ])
        runtime_distilled_again = run([
            sys.executable,
            str(personal_model),
            "distill-runtime",
            "--input",
            str(runtime_training),
            "--model",
            str(pass_through_model),
            "--preferences",
            str(runtime_preferences),
        ])
        runtime_preference_text = runtime_preferences.read_text(encoding="utf-8")
        require(
            "runtime-keyboard-safe\t1" in runtime_distilled.stdout
            and "runtime-correction-pattern-candidates\t1" in runtime_distilled.stdout
            and "runtime-correction-pattern-updated\t1" in runtime_distilled.stdout
            and "runtime-key-habit-candidates\t1" in runtime_distilled.stdout
            and "runtime-key-habit-updated\t1" in runtime_distilled.stdout
            and "runtime-correction-pattern-updated\t0" in runtime_distilled_again.stdout
            and "runtime-key-habit-updated\t0" in runtime_distilled_again.stdout
            and "__correction_pattern__\tmissing\t\tn\t0\t0\t5\n" in runtime_preference_text
            and "__key_habit__\tmissing\t\tn\t5\n" in runtime_preference_text
            and "__correction__\tihao\tnihao\t2\n" in runtime_preference_text
            and runtime_preferences.stat().st_mode & 0o777 == 0o600,
            "clicked TiP training publishes safe generalized edit-channel rules to normal input idempotently",
        )

        untrusted_auxiliary_samples = [
            raw_sample("260", "alpha", ["alpha"], ["raw-pass-through"]),
            raw_sample("261", "bravo", ["bravo"], ["raw-offer"]),
            raw_sample("262", "charlie", ["charlie"], ["raw-pass-through"]),
        ]
        untrusted_auxiliary_samples[1]["input"]["supervision_mode"] = "pass-through-only"
        untrusted_auxiliary_samples[2]["input"]["supervision_mode"] = "pass-through-only"
        untrusted_auxiliary_samples[2]["target"] = {"action": "escape"}
        untrusted_auxiliary_training = temporary_path / "untrusted-auxiliary-training.jsonl"
        untrusted_auxiliary_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in untrusted_auxiliary_samples),
            encoding="utf-8",
        )
        untrusted_auxiliary_model = temporary_path / "untrusted-auxiliary-model.json"
        untrusted_auxiliary_trained = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(untrusted_auxiliary_training),
            "--output",
            str(untrusted_auxiliary_model),
            "--min-samples",
            "3",
            "--validation-percent",
            "0",
            "--no-pinyin-prior",
        ])
        require(
            "raw-profile-auxiliary-positive\t0" in untrusted_auxiliary_trained.stdout
            and "raw-profile-features\t0" in untrusted_auxiliary_trained.stdout
            and "raw-token-evidence\t0" in untrusted_auxiliary_trained.stdout
            and "raw-profile-safe\t0" in untrusted_auxiliary_trained.stdout,
            "only confirmed pass-through commits become TiP raw-profile auxiliary positives",
        )

        bounded_auxiliary_training = temporary_path / "bounded-auxiliary-training.jsonl"
        bounded_auxiliary_samples = []
        for index in range(520):
            item = raw_sample(
                str(1000 + index), f"word{index}", [f"word{index}"], ["raw-pass-through"]
            )
            item["input"]["supervision_mode"] = "pass-through-only"
            bounded_auxiliary_samples.append(item)
        bounded_auxiliary_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in bounded_auxiliary_samples),
            encoding="utf-8",
        )
        bounded_auxiliary_model = temporary_path / "bounded-auxiliary-model.json"
        bounded_auxiliary_trained = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(bounded_auxiliary_training),
            "--output",
            str(bounded_auxiliary_model),
            "--dimension",
            "1024",
            "--epochs",
            "1",
            "--min-samples",
            "4",
            "--validation-percent",
            "0",
            "--no-pinyin-prior",
        ])
        require(
            "samples\t520" in bounded_auxiliary_trained.stdout
            and "raw-profile-auxiliary-positive\t512" in bounded_auxiliary_trained.stdout
            and "raw-token-evidence\t512" in bounded_auxiliary_trained.stdout
            and "active-raw-token-evidence\t0" in bounded_auxiliary_trained.stdout
            and "raw-profile-safe\t0" in bounded_auxiliary_trained.stdout,
            "TiP bounds English pass-through auxiliary training to the newest 512 samples",
        )

        prefix_training = temporary_path / "prefix-training.jsonl"
        prefix_samples = []
        for index in range(4):
            item = sample(
                str(300 + index),
                "jixuzuo",
                ["继续做", "技需", "继续"],
                ["lookup", "prefix", "prefix"],
                2,
                consumed_prefix=4,
            )
            item["input"]["candidate_metadata"][1]["consumed_prefix"] = 4
            prefix_samples.append(item)
        prefix_training.write_text(
            "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in prefix_samples),
            encoding="utf-8",
        )
        prefix_model = temporary_path / "prefix-model.json"
        prefix_trained = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(prefix_training),
            "--output",
            str(prefix_model),
            "--min-samples",
            "4",
            "--validation-percent",
            "0",
        ])
        prefix_ranked = run(
            [sys.executable, str(personal_model), "predict", "--model", str(prefix_model)],
            stdin=request("jixu", ["技需", "继续"], ["lookup", "lookup"]),
        )
        full_ranked = run(
            [sys.executable, str(personal_model), "predict", "--model", str(prefix_model)],
            stdin=request("jixuzuo", ["继续做", "继续"], ["lookup", "prefix"]),
        )
        require(
            "samples\t4" in prefix_trained.stdout
            and prefix_ranked.stdout.splitlines()[0] == "candidate\t继续"
            and full_ranked.stdout.splitlines()[0] == "candidate\t继续做",
            "prefix selections train the consumed prefix without teaching the full long preedit to prefer the prefix",
        )

        exact_request = request("start", ["开始", "start"], ["lookup", "raw-offer"])
        exact = run(
            [sys.executable, str(personal_model), "predict", "--model", str(model)], stdin=exact_request
        )
        exact_rows = exact.stdout.splitlines()
        require(exact_rows[0] == "candidate\tstart" and set(exact_rows) == {"candidate\t开始", "candidate\tstart"},
                "exact personal English preference reranks only existing candidates")

        correction_request = request(
            "nhao", ["脑号", "你好"], ["lookup", "learned-rank:correction"], correction=True
        )
        generalized = run(
            [sys.executable, str(personal_model), "predict", "--model", str(model), "--explain"],
            stdin=correction_request,
        )
        require(generalized.stdout.splitlines()[0] == "candidate\t你好" and "score\t1\t1" in generalized.stderr,
                "personal model generalizes learned correction-source acceptance")

        neutral_model = temporary_path / "neutral-model.json"
        neutral_model.write_text(
            json.dumps({
                "schema": "tipe.personal-reranker.v1",
                "dimension": 4096,
                "baseline_weight": 0.05,
                "promotion_margin": 0.5,
                "weights": {},
                "correction_patterns": [],
                "training": {
                    "samples": 1,
                    "recommendation": "ready\nmodel-status\tforged\t1",
                    "validation_accuracy": None,
                },
            }) + "\n",
            encoding="utf-8",
        )
        neutral = run(
            [sys.executable, str(personal_model), "predict", "--model", str(neutral_model), "--explain"],
            stdin=request("nihao", ["你好", "你号", "拟好"], ["lookup", "lookup", "lookup"]),
        )
        require(
            neutral.stdout.splitlines() == ["candidate\t你好", "candidate\t你号", "candidate\t拟好"]
            and "decision\tpreserve" in neutral.stderr,
            "a low-confidence personal model preserves the complete original candidate order",
        )
        inconsistent_model = temporary_path / "inconsistent-model.json"
        inconsistent_data = json.loads(neutral_model.read_text(encoding="utf-8"))
        inconsistent_data["training"].update({
            "validation_samples": 25,
            "validation_correct": 22,
            "validation_baseline_correct": 22,
            "recommendation": "ready",
        })
        inconsistent_model.write_text(json.dumps(inconsistent_data) + "\n", encoding="utf-8")
        inconsistent_inspection = run(
            [sys.executable, str(personal_model), "inspect", "--model", str(inconsistent_model)]
        )
        require(
            "training-recommendation\tcollect-more-data" in inconsistent_inspection.stdout
            and "training-recommendation\tready" not in inconsistent_inspection.stdout,
            "inspection derives readiness from validation counts instead of trusting stale metadata",
        )
        active_preference_request = request("nihao", ["你好", "你号"], ["lookup", "lookup"])
        active_preference_request += "preference\tnihao\t你号\t3\n"
        active_preference = run(
            [sys.executable, str(personal_model), "predict", "--model", str(neutral_model), "--explain"],
            stdin=active_preference_request,
        )
        inactive_preference_request = request("nihao", ["你好", "你号"], ["lookup", "lookup"])
        inactive_preference_request += "preference\tnihao\t你号\t1\n"
        inactive_preference = run(
            [sys.executable, str(personal_model), "predict", "--model", str(neutral_model), "--explain"],
            stdin=inactive_preference_request,
        )
        confirmed_chain_request = request("jixuzuo", ["技需做", "继续做"], ["lookup", "lookup"])
        confirmed_chain_request += "segment_chain\tjixuzuo\tjixu\t继续\tzuo\tjixuzuo\t继续做\t1\n"
        confirmed_chain = run(
            [sys.executable, str(personal_model), "predict", "--model", str(neutral_model), "--explain"],
            stdin=confirmed_chain_request,
        )
        require(
            active_preference.stdout.splitlines()[0] == "candidate\t你号"
            and "decision\tpromote" in active_preference.stderr
            and inactive_preference.stdout.splitlines()[0] == "candidate\t你好"
            and "decision\tpreserve" in inactive_preference.stderr
            and confirmed_chain.stdout.splitlines()[0] == "candidate\t继续做"
            and "decision\tpromote" in confirmed_chain.stderr,
            "activated supervised evidence is a bounded prior while inactive preferences remain inert",
        )
        preference_feature, preference_weight = signed_feature_weight("evidence:preference:present", 4096, 8.0)
        segment_feature, segment_weight = signed_feature_weight("evidence:segment-chain:present", 4096, 8.0)
        evidence_model = temporary_path / "evidence-model.json"
        evidence_model.write_text(
            json.dumps({
                "schema": "tipe.personal-reranker.v1",
                "dimension": 4096,
                "baseline_weight": 0.05,
                "promotion_margin": 0.5,
                "weights": {
                    preference_feature: round(preference_weight, 8),
                    segment_feature: round(segment_weight, 8),
                },
                "correction_patterns": [],
                "key_habits": [],
                "training": {"samples": 2, "recommendation": "ready"},
            }) + "\n",
            encoding="utf-8",
        )
        preference_request = request("nihao", ["你好", "你号"], ["lookup", "lookup"])
        preference_request += "preference\tnihao\t你号\t3\n"
        preference_ranked = run(
            [sys.executable, str(personal_model), "predict", "--model", str(evidence_model), "--explain"],
            stdin=preference_request,
        )
        require(
            preference_ranked.stdout.splitlines()[0] == "candidate\t你号"
            and "decision\tpromote" in preference_ranked.stderr,
            "personal model can use supervised preference evidence supplied in the request",
        )
        segment_request = request("jixuzuo", ["继续做", "继续"], ["lookup", "lookup"])
        segment_request += "segment_chain\tjixuzuo\tjixu\t继续\tzuo\tjixuzuo\t继续\t2\n"
        segment_ranked = run(
            [sys.executable, str(personal_model), "predict", "--model", str(evidence_model), "--explain"],
            stdin=segment_request,
        )
        require(
            segment_ranked.stdout.splitlines()[0] == "candidate\t继续"
            and "decision\tpromote" in segment_ranked.stderr,
            "personal model can use supervised segment-chain evidence supplied in the request",
        )
        sequence_feature, sequence_weight = signed_feature_weight(
            "sequence:recent:last:2:cursor-move:Right\x1fcandidate:启动", 4096, 8.0
        )
        sequence_model = temporary_path / "sequence-model.json"
        sequence_model.write_text(
            json.dumps({
                "schema": "tipe.personal-reranker.v1",
                "dimension": 4096,
                "baseline_weight": 0.05,
                "promotion_margin": 0.5,
                "weights": {sequence_feature: round(sequence_weight, 8)},
                "correction_patterns": [],
                "key_habits": [],
                "training": {
                    "samples": 20,
                    "validation_strategy": "capability-isolated-temporal-v3",
                    "validation_samples": 10,
                    "validation_correct": 8,
                    "validation_baseline_correct": 5,
                    "validation_non_leading_samples": 5,
                    "validation_non_leading_correct": 3,
                    "validation_generic_non_leading_samples": 5,
                    "validation_generic_non_leading_correct": 3,
                    "recommendation": "ready",
                },
            }) + "\n",
            encoding="utf-8",
        )
        right_context_request = (
            "protocol\t1\npreedit\tstart\ncandidates\t开始\t启动\n"
            "candidate_metadata\t0\tconsumed_prefix\t0\tsource\tlookup\tscore\t100\n"
            "candidate_metadata\t1\tconsumed_prefix\t0\tsource\tuser\tscore\t99\n"
            "events\tletter:s\tcursor-move:Right\tspace:\n"
        )
        right_context = run(
            [sys.executable, str(personal_model), "predict", "--model", str(sequence_model), "--explain"],
            stdin=right_context_request,
        )
        no_right_context = run(
            [sys.executable, str(personal_model), "predict", "--model", str(sequence_model), "--explain"],
            stdin=request("start", ["开始", "启动"], ["lookup", "user"]),
        )
        require(
            right_context.stdout.splitlines()[0] == "candidate\t启动"
            and "decision\tpromote" in right_context.stderr
            and no_right_context.stdout.splitlines()[0] == "candidate\t开始"
            and "decision\tpreserve" in no_right_context.stderr,
            "personal model uses bounded key order and direction only when that context is present",
        )
        tab_feature, tab_weight = signed_feature_weight(
            "sequence:recent:last:2:cursor-move:Tab\x1fcandidate:制表候选", 4096, 8.0
        )
        tab_model_data = json.loads(sequence_model.read_text(encoding="utf-8"))
        tab_model_data["weights"] = {tab_feature: round(tab_weight, 8)}
        tab_model = temporary_path / "tab-sequence-model.json"
        tab_model.write_text(json.dumps(tab_model_data) + "\n", encoding="utf-8")
        tab_context_request = (
            "protocol\t1\npreedit\tstart\ncandidates\t开始\t制表候选\n"
            "candidate_metadata\t0\tconsumed_prefix\t0\tsource\tlookup\tscore\t100\n"
            "candidate_metadata\t1\tconsumed_prefix\t0\tsource\tuser\tscore\t99\n"
            "events\tletter:s\tcursor-move:Tab\tspace:\n"
        )
        home_context_request = tab_context_request.replace("cursor-move:Tab", "cursor-move:Home")
        tab_context = run(
            [sys.executable, str(personal_model), "predict", "--model", str(tab_model), "--explain"],
            stdin=tab_context_request,
        )
        home_context = run(
            [sys.executable, str(personal_model), "predict", "--model", str(tab_model), "--explain"],
            stdin=home_context_request,
        )
        require(
            tab_context.stdout.splitlines()[0] == "candidate\t制表候选"
            and "decision\tpromote" in tab_context.stderr
            and home_context.stdout.splitlines()[0] == "candidate\t开始"
            and "decision\tpreserve" in home_context.stderr,
            "personal model preserves the identity of every bounded navigation key",
        )
        context_feature, context_weight = signed_feature_weight(
            "context-v3:last:1:v1:ae29dad40e284cf7f2329b78cb420481\x1fcandidate:上下文候选",
            4096,
            8.0,
        )
        context_model_data = json.loads(sequence_model.read_text(encoding="utf-8"))
        context_model_data["feature_version"] = 3
        context_model_data["weights"] = {context_feature: round(context_weight, 8)}
        context_model = temporary_path / "context-model.json"
        context_model.write_text(json.dumps(context_model_data) + "\n", encoding="utf-8")
        context_request = request("shijie", ["世界", "上下文候选"], ["lookup", "user"]) + "context\t你好\n"
        contextual_prediction = run(
            [sys.executable, str(personal_model), "predict", "--model", str(context_model), "--explain"],
            stdin=context_request,
        )
        context_free_prediction = run(
            [sys.executable, str(personal_model), "predict", "--model", str(context_model), "--explain"],
            stdin=request("shijie", ["世界", "上下文候选"], ["lookup", "user"]),
        )
        require(
            contextual_prediction.stdout.splitlines()[0] == "candidate\t上下文候选"
            and context_free_prediction.stdout.splitlines()[0] == "candidate\t世界",
            "live raw context and offline opaque context features share the same feature encoding",
        )
        generic_raw_feature, generic_raw_weight = signed_feature_weight(
            "exact:unseenraw\x1funseenraw", 4096, 8.0
        )
        generic_raw_data = json.loads(sequence_model.read_text(encoding="utf-8"))
        generic_raw_data["weights"] = {generic_raw_feature: round(generic_raw_weight, 8)}
        generic_raw_model = temporary_path / "generic-cannot-unlock-raw.json"
        generic_raw_model.write_text(json.dumps(generic_raw_data) + "\n", encoding="utf-8")
        generic_raw_prediction = run(
            [sys.executable, str(personal_model), "predict", "--model", str(generic_raw_model), "--explain"],
            stdin=request("unseenraw", ["原候选", "unseenraw"], ["lookup", "raw-offer"]),
        )
        require(
            generic_raw_prediction.stdout.splitlines()[0] == "candidate\t原候选"
            and "support\tinsufficient" in generic_raw_prediction.stderr,
            "generic Chinese ranking safety cannot unlock an unvalidated raw-offer capability",
        )
        legacy_validation_model = temporary_path / "legacy-validation-model.json"
        legacy_validation_data = json.loads(sequence_model.read_text(encoding="utf-8"))
        legacy_validation_data["training"].pop("validation_strategy")
        legacy_validation_data["training"].pop("validation_generic_non_leading_samples")
        legacy_validation_data["training"].pop("validation_generic_non_leading_correct")
        legacy_validation_model.write_text(json.dumps(legacy_validation_data) + "\n", encoding="utf-8")
        legacy_validation = run(
            [sys.executable, str(personal_model), "predict", "--model", str(legacy_validation_model), "--explain"],
            stdin=right_context_request,
        )
        require(
            legacy_validation.stdout.splitlines()[0] == "candidate\t开始"
            and "decision\tpreserve" in legacy_validation.stderr,
            "legacy validation metadata remains readable but cannot unlock generic ranking",
        )
        unsafe_feature, unsafe_weight = signed_feature_weight("exact:nihao\x1f逆号", 4096, 8.0)
        unsafe_generic_model = temporary_path / "unsafe-generic-model.json"
        unsafe_generic_model.write_text(
            json.dumps({
                "schema": "tipe.personal-reranker.v1",
                "dimension": 4096,
                "baseline_weight": 0.05,
                "promotion_margin": 0.5,
                "weights": {unsafe_feature: round(unsafe_weight, 8)},
                "pair_evidence": {},
                "correction_patterns": [],
                "key_habits": [],
                "training": {
                    "samples": 162,
                    "validation_strategy": "capability-isolated-temporal-v3",
                    "validation_samples": 31,
                    "validation_correct": 27,
                    "validation_baseline_correct": 25,
                    "validation_non_leading_samples": 6,
                    "validation_non_leading_correct": 2,
                    "validation_generic_non_leading_samples": 6,
                    "validation_generic_non_leading_correct": 2,
                    "recommendation": "ready",
                },
            }) + "\n",
            encoding="utf-8",
        )
        unsafe_generic = run(
            [sys.executable, str(personal_model), "predict", "--model", str(unsafe_generic_model), "--explain"],
            stdin=request("nihao", ["你好", "逆号"], ["lookup", "lookup"]),
        )
        require(
            unsafe_generic.stdout.splitlines()[0] == "candidate\t你好"
            and "decision\tpreserve" in unsafe_generic.stderr
            and "support\tinsufficient" in unsafe_generic.stderr,
            "weak non-leading validation cannot let an unseen generic prediction replace a correct first candidate",
        )
        ambiguous_model = temporary_path / "ambiguous-model.json"
        ambiguous_data = json.loads(neutral_model.read_text(encoding="utf-8"))
        ambiguous_data["key_habits"] = [
            {"kind": "missing", "typed": "", "replacement": "n", "count": 5}
        ]
        ambiguous_model.write_text(json.dumps(ambiguous_data) + "\n", encoding="utf-8")
        ambiguous = run(
            [sys.executable, str(personal_model), "predict", "--model", str(ambiguous_model)],
            stdin=request("lueiu", ["略有", "掠有"], ["lookup", "lookup"]),
        )
        require(
            not any(line.startswith("correction\t") for line in ambiguous.stdout.splitlines()),
            "a global key habit does not guess when multiple complete-pinyin repairs tie",
        )
        prior_model = temporary_path / "prior-model.json"
        prior_data = dict(ambiguous_data)
        prior_data["pinyin_prior"] = {"lueinu": 4, "lueniu": 11}
        prior_model.write_text(json.dumps(prior_data) + "\n", encoding="utf-8")
        prior_ranked = run(
            [sys.executable, str(personal_model), "predict", "--model", str(prior_model)],
            stdin=request("lueiu", ["略有", "掠有"], ["lookup", "lookup"]),
        )
        require(
            "correction\tlueiu\tlueniu" in prior_ranked.stdout.splitlines(),
            "the compact pinyin prior resolves an otherwise tied learned key-habit repair",
        )
        neutral_inspect = run(
            [sys.executable, str(personal_model), "inspect", "--model", str(neutral_model)]
        )
        require(
            "training-samples\t1" in neutral_inspect.stdout
            and "forged" not in neutral_inspect.stdout
            and "training-validation-accuracy" not in neutral_inspect.stdout,
            "model inspection does not forward untrusted metadata into TSV status rows",
        )

        omitted_key_request = request("imen", ["一门", "疑闷"], ["lookup", "lookup"])
        omitted_key = run(
            [sys.executable, str(personal_model), "predict", "--model", str(model)],
            stdin=omitted_key_request,
        )
        require(
            "correction\timen\tnimen" in omitted_key.stdout.splitlines(),
            "personal model applies a repeated omitted-key habit to a different preedit",
        )

        valid_pinyin = run(
            [sys.executable, str(personal_model), "predict", "--model", str(model)],
            stdin=request("ali", ["阿里", "啊里"], ["lookup", "lookup"]),
        )
        require(
            not any(line.startswith("correction\t") for line in valid_pinyin.stdout.splitlines()),
            "generic key habits do not rewrite already complete pinyin",
        )

        extra_key = run(
            [sys.executable, str(personal_model), "predict", "--model", str(model)],
            stdin=request("niyren", ["拟人", "你人"], ["lookup", "lookup"]),
        )
        require(
            "correction\tniyren\tniren" in extra_key.stdout.splitlines(),
            "personal model generalizes validated persisted correction evidence",
        )

        global_key_habit = run(
            [sys.executable, str(personal_model), "predict", "--model", str(model)],
            stdin=request("shag", ["啥个", "沙哥"], ["lookup", "lookup"]),
        )
        require(
            not any(line.startswith("correction\t") for line in global_key_habit.stdout.splitlines()),
            "three missing-key observations do not activate a global cross-position habit",
        )

        language_prior_model = temporary_path / "language-prior-model.json"
        language_prior_data = json.loads(model.read_text(encoding="utf-8"))
        language_prior_data["key_habits"].append(
            {"count": 5, "kind": "missing", "replacement": "i", "typed": ""}
        )
        language_prior_model.write_text(json.dumps(language_prior_data) + "\n", encoding="utf-8")
        language_prior = run(
            [sys.executable, str(personal_model), "predict", "--model", str(language_prior_model)],
            stdin=request("pehe", ["配额和", "票额和"], ["lookup", "lookup"]),
        )
        require(
            "correction\tpehe\tpeihe" in language_prior.stdout.splitlines()
            and not any("penhe" in line for line in language_prior.stdout.splitlines()),
            "the pinyin prior rejects absent generalized repairs and selects the strong real pinyin code",
        )

        raw_english = run(
            [sys.executable, str(personal_model), "predict", "--model", str(model)],
            stdin=request("start", ["start", "开始"], ["raw", "lookup"]),
        )
        require(
            not any(line.startswith("correction\t") for line in raw_english.stdout.splitlines()),
            "personal correction patterns do not rewrite an active learned English candidate",
        )

        direct_request = request("nihao", ["你好", "你号"], ["lookup", "lookup"])
        direct_request += "correction_events\t" + "\t".join(
            f"{event['type']}:{event['text']}" for event in correction_trail("ihao", "nihao")
        ) + "\n"
        direct = run(
            [sys.executable, str(personal_model), "predict", "--model", str(model)],
            stdin=direct_request,
        )
        require(
            "correction\tihao\tnihao" in direct.stdout.splitlines(),
            "personal mode keeps direct delete-and-retype learning available",
        )

        exact_known_request = request("woc", ["我操", "我曹"], ["lookup", "lookup"])
        exact_known_request += "correction\twoc\twocao\t2\n"
        exact_known = run(
            [sys.executable, str(personal_model), "predict", "--model", str(model)],
            stdin=exact_known_request,
        )
        require(
            not any(line.startswith("correction\t") for line in exact_known.stdout.splitlines()),
            "a generic key habit does not compete with an exact learned correction for the current preedit",
        )

        malformed = run(
            [sys.executable, str(personal_model), "predict", "--model", str(model)],
            stdin="protocol\t2\npreedit\tstart\ncandidates\t开始\tstart\n",
            expected=1,
        )
        require("missing protocol 1" in malformed.stderr, "malformed request is rejected")

        bad_model = temporary_path / "bad-model.json"
        bad_model.write_text('{"schema":"wrong","weights":{}}\n', encoding="utf-8")
        invalid = run(
            [sys.executable, str(personal_model), "predict", "--model", str(bad_model)],
            stdin=exact_request,
            expected=1,
        )
        require("unsupported model schema" in invalid.stderr, "invalid model is rejected")

        forged_architecture_model = temporary_path / "forged-architecture-model.json"
        forged_architecture_data = json.loads(neutral_model.read_text(encoding="utf-8"))
        forged_architecture_data["architecture"] = "bad\nmodel-status\tforged"
        forged_architecture_model.write_text(json.dumps(forged_architecture_data) + "\n", encoding="utf-8")
        forged_architecture = run(
            [sys.executable, str(personal_model), "inspect", "--model", str(forged_architecture_model)],
            expected=1,
        )
        require(
            "invalid model architecture" in forged_architecture.stderr
            and "forged" not in forged_architecture.stdout,
            "model architecture metadata cannot forge diagnostic rows",
        )

        forged_name_model = temporary_path / "forged-name-model.json"
        forged_name_data = json.loads(neutral_model.read_text(encoding="utf-8"))
        forged_name_data["name"] = "bad\nmodel-status\tforged"
        forged_name_model.write_text(json.dumps(forged_name_data) + "\n", encoding="utf-8")
        forged_name = run(
            [sys.executable, str(personal_model), "inspect", "--model", str(forged_name_model)],
            expected=1,
        )
        require(
            "invalid model name" in forged_name.stderr and "forged" not in forged_name.stdout,
            "model name metadata cannot forge diagnostic rows",
        )

        malformed_raw_evidence_model = temporary_path / "malformed-raw-evidence-model.json"
        malformed_raw_evidence_data = json.loads(neutral_model.read_text(encoding="utf-8"))
        malformed_raw_evidence_data["raw_token_evidence"] = {"not-a-hash": 3}
        malformed_raw_evidence_model.write_text(
            json.dumps(malformed_raw_evidence_data) + "\n", encoding="utf-8"
        )
        malformed_raw_evidence = run(
            [
                sys.executable,
                str(personal_model),
                "inspect",
                "--model",
                str(malformed_raw_evidence_model),
            ],
            expected=1,
        )
        require(
            "invalid raw token evidence" in malformed_raw_evidence.stderr,
            "malformed raw token memory cannot enter TiP",
        )

        legacy_name_model = temporary_path / "legacy-name-model.json"
        legacy_name_data = json.loads(neutral_model.read_text(encoding="utf-8"))
        legacy_name_data.pop("name", None)
        legacy_name_model.write_text(json.dumps(legacy_name_data) + "\n", encoding="utf-8")
        legacy_name = run(
            [sys.executable, str(personal_model), "inspect", "--model", str(legacy_name_model)],
        )
        require(
            "name\tTiP" in legacy_name.stdout,
            "legacy personal model files are identified as TiP without requiring retraining",
        )

        insufficient = temporary_path / "insufficient.jsonl"
        insufficient.write_text(json.dumps(samples[0], ensure_ascii=False) + "\n", encoding="utf-8")
        not_trained = run(
            [
                sys.executable,
                str(personal_model),
                "train",
                "--input",
                str(insufficient),
                "--output",
                str(temporary_path / "unused.json"),
                "--min-samples",
                "4",
            ],
            expected=1,
        )
        require("need at least 4" in not_trained.stderr, "insufficient evidence does not create a model")

        self_test = run([sys.executable, str(personal_model), "self-test"])
        require(self_test.stdout.strip() == "TiP model ok", "TiP model self-test")

        history = temporary_path / "supervision-training-history.tsv"
        history.write_text("".join(history_record(item) for item in samples), encoding="utf-8")
        wrapper_model = temporary_path / "wrapper-model.json"
        wrapper_home = temporary_path / "home"
        wrapper_home.mkdir()
        stale_bin = wrapper_home / ".local" / "bin"
        stale_bin.mkdir(parents=True)
        for stale_helper in ("tipe-training-export", "tipe-personal-model"):
            stale_path = stale_bin / stale_helper
            stale_path.write_text("#!/usr/bin/env bash\necho stale-installed-helper >&2\nexit 97\n", encoding="utf-8")
            stale_path.chmod(0o755)
        wrapper_env = {
            "HOME": str(wrapper_home),
            "PATH": os.environ.get("PATH", ""),
            "XDG_DATA_HOME": str(temporary_path / "wrapper-data"),
            "XDG_CACHE_HOME": str(temporary_path / "wrapper-cache"),
        }
        dry_run = run(
            [str(personal_trainer), "--history", str(history), "--dry-run"], env=wrapper_env
        )
        require(json.loads(dry_run.stdout)["samples"] == 13 and not wrapper_model.exists(),
                "trainer dry-run reports samples without writing a model")
        invalid_nice_env = dict(wrapper_env)
        invalid_nice_env["TIPE_PERSONAL_TRAIN_NICE"] = "20"
        invalid_nice = run(
            [str(personal_trainer), "--history", str(history), "--dry-run"],
            env=invalid_nice_env,
            expected=2,
        )
        require(
            "TIPE_PERSONAL_TRAIN_NICE must be an integer from 0 to 19" in invalid_nice.stderr,
            "trainer rejects an invalid desktop-friendly nice level",
        )
        locked_model = temporary_path / "locked-model.json"
        locked_model_path = Path(str(locked_model) + ".lock")
        with locked_model_path.open("w", encoding="utf-8") as held_lock:
            fcntl.flock(held_lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            concurrent = run(
                [
                    str(personal_trainer),
                    "--history",
                    str(history),
                    "--output",
                    str(locked_model),
                    "--dimension",
                    "4096",
                    "--pinyin-dictionary",
                    str(pinyin_dictionary),
                ],
                env=wrapper_env,
                expected=1,
            )
        require(
            not locked_model.exists()
            and "another TiP training process is already running" in concurrent.stderr
            and locked_model_path.stat().st_mode & 0o777 == 0o600,
            "concurrent training is rejected before it can overwrite the shared model",
        )
        component_upgrade_history = temporary_path / "component-upgrade-history.tsv"
        component_upgrade_samples = []
        for index in range(10):
            item = sample(str(900 + index), "nihao", ["你好", "你号"], ["lookup", "lookup"], 0)
            item["input"]["correction_events"] = correction_trail("ihao", "nihao")
            component_upgrade_samples.append(item)
        component_upgrade_history.write_text(
            "".join(history_record(item) for item in component_upgrade_samples), encoding="utf-8"
        )
        component_upgrade_model = temporary_path / "component-upgrade-model.json"
        component_upgrade_data = json.loads(neutral_model.read_text(encoding="utf-8"))
        component_upgrade_data["pair_evidence"] = {"f" * 24: 7}
        component_upgrade_data["raw_token_evidence"] = {"e" * 24: 6}
        component_upgrade_data["correction_patterns"] = [{
            "kind": "missing",
            "typed": "",
            "replacement": "z",
            "position": 0,
            "relative_to_end": False,
            "count": 5,
        }]
        component_upgrade_data["key_habits"] = [{
            "kind": "replace", "typed": "x", "replacement": "z", "count": 6,
        }]
        component_upgrade_data["pinyin_prior"] = {"nihao": 10}
        component_upgrade_model.write_text(
            json.dumps(component_upgrade_data, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        component_upgrade = run(
            [
                str(personal_trainer),
                "--history",
                str(component_upgrade_history),
                "--output",
                str(component_upgrade_model),
                "--dimension",
                "4096",
                "--pinyin-dictionary",
                str(pinyin_dictionary),
            ],
            env=wrapper_env,
        )
        component_upgrade_inspect = run(
            [sys.executable, str(personal_model), "inspect", "--model", str(component_upgrade_model)]
        )
        component_upgrade_merged = json.loads(component_upgrade_model.read_text(encoding="utf-8"))
        require(
            "recommendation\tcollect-more-data" in component_upgrade.stdout
            and "component-update-safe\t1" in component_upgrade.stdout
            and "model-updated\t1" in component_upgrade.stdout
            and "model-update-kind\tsafe-component-validation-upgrade" in component_upgrade.stdout
            and "merge-strategy\tmax-count-monotonic-v1" in component_upgrade.stdout
            and "restored-active-pair-evidence\t1" in component_upgrade.stdout
            and "restored-active-raw-token-evidence\t1" in component_upgrade.stdout
            and "restored-active-correction-patterns\t1" in component_upgrade.stdout
            and "restored-active-key-habits\t1" in component_upgrade.stdout
            and "feature-version\t4" in component_upgrade_inspect.stdout
            and "name\tTiP" in component_upgrade_inspect.stdout
            and "evidence-merge-strategy\tmax-count-monotonic-v1" in component_upgrade_inspect.stdout
            and component_upgrade_merged["name"] == "TiP"
            and component_upgrade_merged["pair_evidence"]["f" * 24] == 7
            and component_upgrade_merged["raw_token_evidence"]["e" * 24] == 6
            and any(row["replacement"] == "z" for row in component_upgrade_merged["correction_patterns"])
            and any(row["replacement"] == "z" for row in component_upgrade_merged["key_habits"]),
            "a safe component upgrade keeps active evidence from the existing bounded-history model\n"
            + component_upgrade.stdout
            + component_upgrade_inspect.stdout,
        )

        component_regression_samples = []
        for index in range(5):
            item = sample(
                str(920 + index), "flip", ["默认", "学习"], ["lookup", "lookup"], 1
            )
            item["input"]["correction_events"] = correction_trail("lip", "flip")
            component_regression_samples.append(item)
        for index in range(5):
            item = sample(
                str(930 + index),
                f"novel{index}",
                [f"默认新项{index}", f"未见新项{index}"],
                ["lookup", "lookup"],
                1,
            )
            component_regression_samples.append(item)
        for index in range(19):
            item = sample(
                str(940 + index),
                f"stable{index}",
                [f"稳定{index}", f"备选{index}"],
                ["lookup", "lookup"],
                0,
            )
            component_regression_samples.append(item)
        item = sample("970", "flip", ["默认", "学习"], ["lookup", "lookup"], 0)
        item["input"]["correction_events"] = correction_trail("lip", "flip")
        component_regression_samples.append(item)

        component_regression_training = temporary_path / "component-regression.jsonl"
        component_regression_training.write_text(
            "".join(
                json.dumps(item, ensure_ascii=False) + "\n"
                for item in component_regression_samples
            ),
            encoding="utf-8",
        )
        component_regression_candidate = temporary_path / "component-regression-candidate.json"
        component_regression_train = run([
            sys.executable,
            str(personal_model),
            "train",
            "--input",
            str(component_regression_training),
            "--output",
            str(component_regression_candidate),
            "--dimension",
            "4096",
            "--pinyin-dictionary",
            str(pinyin_dictionary),
        ])
        component_regression_data = json.loads(
            component_regression_candidate.read_text(encoding="utf-8")
        )
        require(
            component_regression_data["training"]["validation_correct"]
                < component_regression_data["training"]["validation_baseline_correct"]
            and component_regression_data["training"]["generic_ranking_safe"] is False
            and component_regression_data["training"]["keyboard_correction_safe"] is True
            and component_regression_data["training"]["component_update_safe"] is True
            and "recommendation\tkeep-heuristic" in component_regression_train.stdout
            and "generic-ranking-safe\t0" in component_regression_train.stdout
            and "keyboard-correction-safe\t1" in component_regression_train.stdout
            and "component-update-safe\t1" in component_regression_train.stdout,
            "generic ranking regression cannot block an independently safe keyboard component\n"
            + component_regression_train.stdout,
        )

        component_regression_history = temporary_path / "component-regression-history.tsv"
        component_regression_history.write_text(
            "".join(history_record(item) for item in component_regression_samples),
            encoding="utf-8",
        )
        component_regression_model = temporary_path / "component-regression-model.json"
        component_regression_model.write_bytes(component_upgrade_model.read_bytes())
        component_regression_publish = run(
            [
                str(personal_trainer),
                "--history",
                str(component_regression_history),
                "--output",
                str(component_regression_model),
                "--dimension",
                "4096",
                "--pinyin-dictionary",
                str(pinyin_dictionary),
            ],
            env=wrapper_env,
        )
        component_regression_merged = json.loads(
            component_regression_model.read_text(encoding="utf-8")
        )
        require(
            "recommendation\tkeep-heuristic" in component_regression_publish.stdout
            and "component-update-safe\t1" in component_regression_publish.stdout
            and "model-updated\t1" in component_regression_publish.stdout
            and "model-update-kind\tsafe-component-upgrade" in component_regression_publish.stdout
            and "merge-strategy\tmax-count-monotonic-v1" in component_regression_publish.stdout
            and component_regression_merged["training"]["generic_ranking_safe"] is False
            and component_regression_merged["training"]["keyboard_correction_safe"] is True
            and component_regression_merged["pair_evidence"]["f" * 24] == 7
            and component_regression_merged["raw_token_evidence"]["e" * 24] == 6
            and any(
                row["replacement"] == "z"
                for row in component_regression_merged["correction_patterns"]
            )
            and any(
                row["replacement"] == "z"
                for row in component_regression_merged["key_habits"]
            ),
            "safe keyboard learning publishes despite generic regression and preserves old evidence\n"
            + component_regression_publish.stdout,
        )

        validation_upgrade_model = temporary_path / "validation-upgrade-model.json"
        validation_upgrade_data = json.loads(component_upgrade_model.read_text(encoding="utf-8"))
        validation_upgrade_data["training"]["validation_strategy"] = "stratified-temporal-v1"
        for key in list(validation_upgrade_data["training"]):
            if key.startswith("validation_generic_"):
                validation_upgrade_data["training"].pop(key)
        validation_upgrade_model.write_text(
            json.dumps(validation_upgrade_data, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        validation_upgrade = run(
            [
                str(personal_trainer),
                "--history",
                str(component_upgrade_history),
                "--output",
                str(validation_upgrade_model),
                "--dimension",
                "4096",
                "--pinyin-dictionary",
                str(pinyin_dictionary),
            ],
            env=wrapper_env,
        )
        validation_upgrade_inspect = run(
            [sys.executable, str(personal_model), "inspect", "--model", str(validation_upgrade_model)]
        )
        require(
            "recommendation\tcollect-more-data" in validation_upgrade.stdout
            and "model-updated\t1" in validation_upgrade.stdout
            and "model-update-kind\tsafe-component-validation-upgrade" in validation_upgrade.stdout
            and "training-validation-strategy\tcapability-isolated-temporal-v4" in validation_upgrade_inspect.stdout
            and "generic-ranking-safe\t0" in validation_upgrade_inspect.stdout,
            "a same-feature legacy validation model migrates without unlocking generic ranking",
        )
        nice_bin = temporary_path / "nice-bin"
        nice_bin.mkdir()
        nice_marker = temporary_path / "nice-marker"
        fake_nice = nice_bin / "nice"
        fake_nice.write_text(
            "#!/bin/sh\n"
            "printf '%s\\n' \"$1 $2\" >\"$TIPE_TEST_NICE_MARKER\"\n"
            "shift 2\n"
            "exec \"$@\"\n",
            encoding="utf-8",
        )
        fake_nice.chmod(0o755)
        nice_wrapper_env = dict(wrapper_env)
        nice_wrapper_env["PATH"] = str(nice_bin) + os.pathsep + wrapper_env["PATH"]
        nice_wrapper_env["TIPE_TEST_NICE_MARKER"] = str(nice_marker)
        wrapper_train = run(
            [
                str(personal_trainer),
                "--history",
                str(history),
                "--output",
                str(wrapper_model),
                "--epochs",
                "10",
                "--dimension",
                "4096",
                "--pinyin-dictionary",
                str(pinyin_dictionary),
            ],
            env=nice_wrapper_env,
        )
        require(wrapper_model.is_file() and "activation-hint\t" in wrapper_train.stdout and
                    "pinyin-prior-entries\t8" in wrapper_train.stdout and
                    "model-updated\t1" in wrapper_train.stdout and
                    nice_marker.read_text(encoding="utf-8").strip() == "-n 10" and
                    Path(str(wrapper_model) + ".lock").stat().st_mode & 0o777 == 0o600 and
                    "note\tThe model configuration was not changed" in wrapper_train.stdout,
                "one-shot trainer writes the model without changing active configuration")
        blocked_preferences_parent = temporary_path / "blocked-preferences-parent"
        blocked_preferences_parent.write_text("not a directory\n", encoding="utf-8")
        postprocess_failure_model = temporary_path / "postprocess-failure-model.json"
        postprocess_failure = run(
            [
                str(personal_trainer),
                "--history",
                str(history),
                "--output",
                str(postprocess_failure_model),
                "--dimension",
                "4096",
                "--pinyin-dictionary",
                str(pinyin_dictionary),
                "--preferences",
                str(blocked_preferences_parent / "candidate-preferences.tsv"),
            ],
            env=wrapper_env,
        )
        require(
            postprocess_failure_model.is_file()
            and "model-updated\t1" in postprocess_failure.stdout
            and "runtime-distill\tfailed" in postprocess_failure.stdout
            and "runtime preference synchronization will retry next time" in postprocess_failure.stdout,
            "an optional runtime synchronization failure cannot misreport a published TiP model as failed",
        )
        generic_safe_model = temporary_path / "generic-safe-wrapper-model.json"
        generic_safe_model.write_bytes(stratified_model.read_bytes())
        generic_safe_bytes = generic_safe_model.read_bytes()
        generic_downgrade_history = temporary_path / "generic-downgrade-history.tsv"
        generic_downgrade_history.write_text(
            "".join(history_record(item) for item in seen_preedit_samples), encoding="utf-8"
        )
        generic_safe_preserved = run(
            [
                str(personal_trainer),
                "--history",
                str(generic_downgrade_history),
                "--output",
                str(generic_safe_model),
                "--dimension",
                "4096",
                "--pinyin-dictionary",
                str(pinyin_dictionary),
            ],
            env=wrapper_env,
        )
        require(
            generic_safe_model.read_bytes() == generic_safe_bytes
            and "recommendation\tready" in generic_safe_preserved.stdout
            and "generic-ranking-safe\t0" in generic_safe_preserved.stdout
            and "model-updated\t0" in generic_safe_preserved.stdout
            and "model-update-kind\tpreserved-safe-capability" in generic_safe_preserved.stdout,
            "a ready candidate cannot replace an existing model when it would relock generic ranking",
        )
        wrapper_generalized = run(
            [sys.executable, str(personal_model), "predict", "--model", str(wrapper_model)],
            stdin=request("imen", ["一门", "疑闷"], ["lookup", "lookup"]),
        )
        require(
            "correction\timen\tnimen" in wrapper_generalized.stdout.splitlines(),
            "terminal history preserves delete-and-retype evidence for generalized correction habits",
        )
        component_only_existing = json.loads(wrapper_model.read_text(encoding="utf-8"))
        component_only_training = component_only_existing["training"]
        component_only_training["validation_correct"] = component_only_training[
            "validation_baseline_correct"
        ]
        component_only_training["validation_gain"] = 0
        component_only_training["recommendation"] = "collect-more-data"
        component_only_training["generic_ranking_safe"] = False
        wrapper_model.write_text(
            json.dumps(component_only_existing, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        ready_model_bytes = wrapper_model.read_bytes()
        preserved = run(
            [
                str(personal_trainer),
                "--history",
                str(history),
                "--output",
                str(wrapper_model),
                "--validation-percent",
                "0",
            ],
            env=wrapper_env,
        )
        require(
            wrapper_model.read_bytes() != ready_model_bytes
            and "recommendation\tcollect-more-data" in preserved.stdout
            and "model-updated\t1" in preserved.stdout
            and "model-update-kind\tsafe-component-upgrade" in preserved.stdout
            and "merge-strategy\tmax-count-monotonic-v1" in preserved.stdout,
            "trainer publishes safe exact and correction evidence even before generic ranking is ready\n"
            + preserved.stdout,
        )
        forced = run(
            [
                str(personal_trainer),
                "--history",
                str(history),
                "--output",
                str(wrapper_model),
                "--validation-percent",
                "0",
                "--force",
            ],
            env=wrapper_env,
        )
        require(
            wrapper_model.read_bytes() != ready_model_bytes and "model-updated\t1" in forced.stdout,
            "explicit force replaces an existing personal model despite its recommendation",
        )
        require(not (wrapper_home / ".config" / "tipe" / "model-env").exists(),
                "one-shot trainer does not activate personal mode")

    print("personal model integration ok")


if __name__ == "__main__":
    main()
