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


def run(command, *, stdin="", env=None):
    result = subprocess.run(
        command,
        input=stdin,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        check=False,
    )
    require(result.returncode == 0, f"unexpected exit {result.returncode}: {result.stderr}")
    return result.stdout


def main():
    require(len(sys.argv) == 4, "expected model-explain, model-adapter, and training-export paths")
    model_explain = Path(sys.argv[1])
    model_adapter = Path(sys.argv[2])
    training_export = Path(sys.argv[3])
    for path in (model_explain, model_adapter, training_export):
        require(path.is_file(), f"missing helper: {path}")

    correction_events = (
        [f"letter:{character}" for character in "ihao"]
        + ["backspace:" for _ in "ihao"]
        + [f"letter:{character}" for character in "nihao"]
        + ["space:", "raw-committed:nihao"]
    )
    request = "\n".join([
        "protocol\t1",
        "preedit\tnihao",
        "application\tEditor",
        "candidates\tnihao",
        "candidate_metadata\t0\tconsumed_prefix\t0\tsource\traw-pass-through\tscore\t900000",
        "state\tpreedit_cursor\t5\tcandidate_cursor\t0\texpanded\t0",
        "runtime_state\tcontinuous\t0\tinput_mode\tenglish",
        "supervision_state\tmode\tpass-through-only\tactive_preedit\t0\trecent_events\t15\tcorrection_events\t15",
        "selected_candidate\t0\tnihao",
        "visible_candidates",
        "numbered_candidates",
        "events\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\tspace:\traw-committed:nihao",
        "event_counts\tletter:5\tspace:1\traw-committed:1",
        "correction_events\t" + "\t".join(correction_events),
        "correction_event_counts\tletter:9\tbackspace:4\tspace:1\traw-committed:1",
        "context",
        "",
    ])

    with tempfile.TemporaryDirectory(prefix="tipe-pass-through-pipeline-") as temporary:
        root = Path(temporary)
        request_path = root / "request.tsv"
        request_path.write_text(request, encoding="utf-8")

        panel = run([str(model_explain), "--panel", str(request_path)])
        require(
            "panel\tsupervision\tmode\tpass-through-only" in panel
            and "panel\tsupervision\tinput-mode\tenglish" in panel,
            "learning panel preserves declared English pass-through mode",
        )

        adapter_output = run(
            [str(model_adapter)],
            stdin=request,
            env={
                **os.environ,
                "TIPE_MODEL_BACKEND": "openai-compatible",
                "TIPE_MODEL_DRY_RUN": "1",
            },
        )
        request_json_line = next(
            (line.removeprefix("request-json\t") for line in adapter_output.splitlines()
             if line.startswith("request-json\t")),
            "",
        )
        require(request_json_line, "adapter dry-run emits request JSON")
        payload = json.loads(request_json_line)
        prompt = json.loads(payload["messages"][-1]["content"])
        require(
            prompt["supervision_mode"] == "pass-through-only"
            and prompt["runtime_state"]["input_mode"] == "english"
            and prompt["preedit"] == "nihao",
            "cloud prompt identifies bounded English pass-through input",
        )
        require(
            prompt["behavior_summary"]["possible_corrections"] == [{
                "source": "full-delete-retype",
                "typo": "ihao",
                "corrected_preedit": "nihao",
            }],
            "cloud prompt retains pass-through delete-and-retype evidence",
        )

        llama_model = root / "qwen.gguf"
        llama_model.write_bytes(b"test-model")
        llama_prompt_path = root / "llama-prompt-path"
        llama_cli = root / "llama-cli"
        llama_cli.write_text(
            "#!/usr/bin/env bash\n"
            "set -euo pipefail\n"
            "model=\n"
            "prompt=\n"
            "while [[ $# -gt 0 ]]; do\n"
            "  case \"$1\" in\n"
            "    -m) model=$2; shift 2 ;;\n"
            "    -f) prompt=$2; shift 2 ;;\n"
            "    *) shift ;;\n"
            "  esac\n"
            "done\n"
            "[[ $model == \"$TIPE_TEST_LLAMA_MODEL\" && -r $prompt ]]\n"
            "printf '%s' \"$prompt\" >\"$TIPE_TEST_LLAMA_PROMPT_PATH\"\n"
            "grep -q '\"supervision_mode\": \"pass-through-only\"' \"$prompt\"\n"
            "printf 'candidate\\tnihao\\ncorrection\\tihao\\tnihao\\ncandidate\\tforged\\n'\n",
            encoding="utf-8",
        )
        llama_cli.chmod(0o755)
        llama_output = run(
            [str(model_adapter)],
            stdin=request,
            env={
                **os.environ,
                "TIPE_MODEL_BACKEND": "llama-cpp",
                "TIPE_MODEL_NAME": str(llama_model),
                "TIPE_LLAMA_CPP_COMMAND": str(llama_cli),
                "TIPE_TEST_LLAMA_MODEL": str(llama_model),
                "TIPE_TEST_LLAMA_PROMPT_PATH": str(llama_prompt_path),
            },
        )
        require(
            llama_output.splitlines() == ["candidate\tnihao", "correction\tihao\tnihao"],
            "on-demand llama.cpp output passes through the same candidate and correction safety filter",
        )
        require(
            llama_prompt_path.is_file()
            and not Path(llama_prompt_path.read_text(encoding="utf-8")).exists(),
            "on-demand llama.cpp removes its private prompt directory after the process exits",
        )
        llama_dry_run = run(
            [str(model_adapter)],
            stdin=request,
            env={
                **os.environ,
                "TIPE_MODEL_BACKEND": "llama-cpp",
                "TIPE_MODEL_NAME": str(llama_model),
                "TIPE_LLAMA_CPP_COMMAND": str(llama_cli),
                "TIPE_MODEL_DRY_RUN": "1",
            },
        )
        require(
            "request\tllama-cpp:" + str(llama_model) in llama_dry_run
            and "request-json\t" in llama_dry_run
            and "llama-command\t" + str(llama_cli) in llama_dry_run,
            "llama.cpp dry-run exposes the prompt and exact one-shot command without loading a model",
        )

        history = root / "history.tsv"
        history.write_text(
            "---\tunix_ms\t1\tprogram\tEditor\tpreedit\tnihao\tcandidates\t1\texpanded\t0\tterminal\t1\n"
            + request,
            encoding="utf-8",
        )
        exported = run([
            sys.executable,
            str(training_export),
            "--history",
            str(history),
            "--include-correction-trail",
        ])
        samples = [json.loads(line) for line in exported.splitlines() if line]
        require(
            len(samples) == 1
            and samples[0]["task"] == "raw-committed"
            and samples[0]["input"]["supervision_mode"] == "pass-through-only"
            and samples[0]["input"]["candidates"] == ["nihao"]
            and samples[0]["input"]["correction_events"][-1] == {"type": "space", "text": ""},
            "training export retains one-candidate English correction supervision",
        )

    print("pass-through pipeline ok")


if __name__ == "__main__":
    main()
