# Debugging

Normal TiPE input does not run a model in the background. Every delivered key press relevant to supervision stays in the bounded in-memory trail; release events are ignored except for model-trigger debouncing. Active-request serialization and disk replacement are coalesced to at most once per 250 ms, non-terminal general history is sampled at most once per 2 seconds, and terminal training records are always retained immediately. While the supervision window is open, per-key UI refresh reuses cached history/model metadata and periodically refreshes that context instead of rerunning the full analysis stack every second.

Inspect TiPE's internal mode without changing it:

```bash
tipe-toggle --status
tipe-doctor | rg 'input-mode|current input method'
```

`tipe-toggle` must keep fcitx5 active with `tipe` selected. The mode file normally lives at `$XDG_RUNTIME_DIR/tipe/input-mode`; a missing or invalid file means Chinese mode. A successful request has the same mode and unique token in the sibling `input-mode-applied` file. If they differ, the helper must report failure instead of treating a transient `fcitx5-remote -n` result as success. In English mode, `supervision-current.tsv` should contain `supervision_state<TAB>mode<TAB>pass-through-only` and a `runtime_state` field pair `input_mode<TAB>english`. Its nonempty `preedit` is only the bounded current English token, not captured application text.

If Chinese/English switching appears to work but no `pass-through-only` records are created, run `tipe-doctor` and inspect `niri-mode-toggle`. A niri binding such as `Mod+Space { spawn-sh "fcitx5-remote -t"; }` is incompatible with supervision because it deactivates TiPE. The supported binding is `Mod+Space { spawn-sh "$HOME/.local/bin/tipe-toggle"; }`. The doctor check is read-only.

Useful commands:

```bash
./scripts/build.sh
./scripts/smoke-test.sh
./scripts/install.sh
./scripts/uninstall.sh --dry-run
./build/tipe-state-probe nihao
./build/tipe-state-probe nihao --dictionary examples/user-dictionary.tsv
./build/tipe-state-probe shunxuyoudianwenti --select 顺序
./build/tipe-state-probe nihao --digit 2
./build/tipe-state-probe nihao --move Down --move Down --digit 1
./build/tipe-state-probe nihao --punct ,
./build/tipe-state-probe nihao --space
./build/tipe-state-probe nihao --backspace --delete --escape
./build/tipe-state-probe nihao --observe Tab --move Down --rerank --events
./build/tipe-state-probe nihao --application Alacritty --surrounding-before '刚才' --rerank
./build/tipe-state-probe nihao --key ISO_Left_Tab
./build/tipe-state-probe nihao --preferences /tmp/tipe-preferences.tsv --select 你号
./build/tipe-state-probe nihao --move Down --snapshot 100,200,3,18
./build/tipe-state-probe "" --script examples/probe-actions.txt
printf '%s\n' 'type nihao' 'select 你' 'expect-preedit hao' | ./build/tipe-state-probe "" --script -
printf '%s\n' 'type nihao' 'select 你' 'select 好' > /tmp/tipe-actions.txt
./build/tipe-state-probe "" --script /tmp/tipe-actions.txt
./scripts/check-user-dictionary.sh examples/user-dictionary.tsv
./scripts/check-user-dictionary.sh --add dgithubdeshihou 打github的时候 --path /tmp/tipe-user-dictionary.tsv
./scripts/check-user-dictionary.sh --add nihao 你好 --first --path /tmp/tipe-user-dictionary.tsv
./scripts/check-preferences.sh /tmp/tipe-preferences.tsv
./scripts/check-preferences.sh --explain /tmp/tipe-preferences.tsv
./scripts/check-preferences.sh --summary --top 5 /tmp/tipe-preferences.tsv
./scripts/doctor.sh --no-runtime
TIPE_MODEL_COMMAND=./scripts/model-protocol-example.sh ./build/tipe-state-probe nihao --rerank
TIPE_MODEL_COMMAND=./scripts/model-dump.sh TIPE_MODEL_DUMP_PATH=/tmp/tipe-model.tsv ./build/tipe-state-probe nihao --application Alacritty --rerank
./build/tipe-candidate-window --self-test
./build/tipe-candidate-window --parse-snapshot $'nihao\t1\t7\t100\t200\t3\t18\t你好|你号|拟好|倪浩|泥豪|你好啊|你不好|你很好'
./build/tipe-candidate-window --parse-snapshot $'chang\t0\t0\t100\t200\t3\t18\t长句先选前半段后|短|后面的拼音不保留|候选|另外|更多'
./build/tipe-candidate-window --parse-snapshot $'nihao\t1\t7\t1240\t700\t2\t18\t你好|你号|拟好|倪浩|泥豪|你好啊|你不好|你很好' --layout-geometry 0,0,1280,720
./build/tipe-candidate-window --parse-snapshot $'nihao\t0\t0\t0\t0\t0\t0\t你好|你号' --layout-geometry 0,0,1280,720
./build/tipe-state-probe nihao --move Down --snapshot 100,200,3,18 | awk -F '\t' '$1 == "snapshot" {sub(/^snapshot\t/, ""); print; exit}' | ./build/tipe-candidate-window --parse-snapshot -
```

`--digit N` follows real digit-key behavior: it selects a numbered candidate for normal pinyin and inserts the digit when it continues an active raw/alphanumeric token such as `qwen2`.

`--script` accepts one action per line: `type`, `select`, `key`, `move`, `digit`, `observe`, `rerank`, `continuous-mode`, `continuous-rerank`, `punct`, `space`, `enter`, `backspace`, `delete`, or `escape`. `key` replays one engine-style key name such as `KP_Down`, `ISO_Left_Tab`, `F9`, `KP_Enter`, or `KP_Add`. `--continuous-mode` marks the diagnostic UI state as continuous mode, `continuous-mode on|off` toggles that script-local flag, and `continuous-rerank` simulates the continuous-mode lightweight path: it records a rerank event, marks later snapshots as continuous, keeps the panel collapsed, and avoids `TIPE_MODEL_COMMAND`. `move` accepts `Down`, `Up`, `PageDown`, `PageUp`, `Home`, `End`, `Left`, `Right`, `Tab`, and `ShiftTab`, plus matching keypad and fcitx aliases such as `KP_Down`, `KP_Page_Down`, `Next`, `Prior`, and `ISO_Left_Tab`. It also supports `expect-preedit`, `expect-preedit-cursor`, `expect-candidate`, `expect-visible`, `expect-event`, `expect-has-candidate`, `expect-context`, `expect-expanded`, and `expect-selected` assertions. Use `expect-preedit-cursor INDEX` to distinguish pinyin editing from candidate movement, `expect-visible DIGIT INDEX TEXT` to check the row-local number labels shown in the current collapsed or expanded candidate row, and `expect-event INDEX KIND [TEXT]` to check the ordered key trail that model requests can use. It is the preferred way to turn long real-use feedback into a reproducible state-machine check without switching the active input method.

Use `--preferences PATH` when checking whether learned candidate order, typo correction, or segment-chain suffix reranking survives across separate probe processes. This keeps the test isolated from the real TiPE user data while still exercising the same persisted preference format.
Use `tipe-check-user-dictionary --explain` to inspect automatically learned whole phrases. A phrase selected in multiple pieces is intentionally absent after its first completed chain and appears only after the second identical confirmation. User-dictionary writes take the same `.lock` as `--add`, replace the TSV atomically, use mode `0600`, and are visible to the next lookup without restarting fcitx5.
Use `tipe-check-preferences --summary --top N PATH` to inspect learned candidate preferences, raw/legacy preferences, correction rows, and segment-selection chains by strength without editing the file. Candidate ranking activates at count 2 and raw preference at count 3; lower candidate counts remain visible as `inactive-evidence`. Local correction borrowing also requires count 2, but one-count correction rows remain in model requests so a clicked model can judge that weak evidence without the local path applying it automatically. The model adapter reapplies candidate-preference thresholds at its request boundary, so a hand-written, replayed, or older request cannot expose inactive candidate rows to heuristic ranking or HTTP `known_preferences`. A later explicit candidate decision adds to retained inactive preference evidence instead of being blocked by its mere presence. Segment-chain rows with an implied correction are validated the same way as correction rows, so implausible chains are rejected instead of being loaded into ranking.
Exact correction rows activate local typo borrowing at count two. Generic positional patterns activate at 2 observations for missing/transposed keys, 3 for extra keys, and 4 for replacements; global habits require 3 for transposition, 5 for missing, and 6 for extra/replacement. They are normally published by clicked TiP training after those safety thresholds; old files without published rules retain the legacy repeated-pair fallback. `tipe-check-preferences --summary` reports both the raw correction-derived summary and the active count for `runtime-correction-patterns` / `runtime-key-habits` rows.

Use `tipe-check-preferences --preedit TEXT PATH` when one input string behaves oddly. Besides the matching stored rows, it prints `query-effect` rows that say whether the next effect is a ranking boost, raw-English preference, typo-correction borrow, or segment-chain suffix continuation. A `query-inactive-evidence` row is retained history below its activation threshold and is not a current ranking effect.
Add `--query-only` for the live-window hot path when only matching rows need validation; leave it off when auditing the integrity of the complete preference file.
Use `--application NAME` when checking the explicit model-rerank path. It simulates the focused application string that the fcitx5 engine passes to model wrappers. Use `--surrounding-before TEXT` and `--surrounding-after TEXT` to simulate bounded client-provided text around the cursor.

`--request` prints the raw model request TSV for the final probe state. For example, `./build/tipe-state-probe "" --observe start --observe Left --space --request` verifies that empty-preedit pass-through supervision reaches the model as `supervision_state mode pass-through-only` with the observed keys and space event, without opening any window or calling a model.
`--learned-corrections` prints the bounded corrected-pinyin stream and dictionary priority used by ordinary input. Use it with an isolated `--preferences PATH` to diagnose TiP habits without touching user data. `--snapshot X,Y,W,H` prints the exact candidate-window protocol line generated from the final probe state, including supervision, continuous-mode, and preedit-cursor metadata. Piping that row into `tipe-candidate-window --parse-snapshot -` checks the state-to-window boundary without opening GTK.
`tipe-candidate-window --parse-snapshot ... --layout-geometry X,Y,W,H` also prints estimated window size and clamped global-screen position, so cursor-edge placement can be checked without opening GTK, including monitors whose origin is not `0,0`. A `layout` row ending in `0` means the supplied snapshot rectangle itself is unusable; runtime metadata may still request the XIM pointer fallback. A `draw-cell` row prints candidate bounds and a final `1` when they stay inside panel padding. An `edge-fallback` row is actionable only for known global coordinates; input-method-v2 `text-rect` is surface-local. Internal `TiPE`/`Eng` status stays on the compositor popup for `wayland_v2`. XIM, DBus, and other frontends consume the same fcitx input panel through one persistent GTK fallback; an empty snapshot hides it, and a later composition reuses it. They do not also publish a second native panel.

The normal UI owner is `tipeui`: it uses its native popup for `wayland_v2` and the visually identical GTK fallback for other frontends. Only run the next commands when you intentionally want to restart fcitx5 and switch the active input method. The restart helper itself tries to activate TiPE:

```bash
./scripts/restart-fcitx5.sh --dry-run
./scripts/restart-fcitx5.sh --debug --ui tipeui
fcitx5-remote -n
fcitx5-remote -m tipe
journalctl --user -u fcitx5.service -n 120 --no-pager
TIPE_LOG_DIR="${XDG_CACHE_HOME:-"$HOME/.cache"}/tipe"
# Only for the direct fallback launcher:
rg 'TiPE addon loaded|tipe' "$TIPE_LOG_DIR/fcitx5.log"
tail -n 120 "$TIPE_LOG_DIR/tipeui.log"
```

If the log shows `TiPE addon loaded` and `fcitx5-remote -m tipe` prints `tipe`, but `fcitx5-remote -n` stays on another input method after `fcitx5-remote -s tipe`, the installed addon is present but the active fcitx5 profile may not include TiPE. Do not edit `~/.config/fcitx5/profile` as part of routine debugging; that changes the current input environment and should be done only after the user explicitly asks for it.

`tipe-restart-fcitx5` is a current-user session helper and requires `HOME` to be set. Its dry run identifies both log destinations: user-service launches go to `journalctl --user -u fcitx5.service`, while only the direct fallback writes `fcitx5.log`. With `--model-current`, it also prints the model config path, whether that file is present, passes configured continuous-mode and model-timeout values into fcitx5, and prints a `tipe-doctor` runtime verification hint. When a user `fcitx5.service` is available, the helper passes TiPE's transient variables through `systemctl --user set-environment`, stops any old instance, and starts that service without editing its unit file. It records the first active `MainPID` and rechecks the unit state, unchanged PID, and single-process identity around every `fcitx5-remote` call. A service crash, systemd restart, or D-Bus-activated replacement therefore fails the helper instead of being mistaken for success. Direct `nohup` launch remains a fallback when no service is available or custom `--ui` arguments are requested. A real restart removes `$XDG_CACHE_HOME/tipe/supervision-current.tsv`, or `~/.cache/tipe/supervision-current.tsv` when `XDG_CACHE_HOME` is unset, so old live supervision is not mistaken for a new active composition; `supervision-last.tsv` is left in place for review. After restart it asks fcitx5 to activate TiPE, verifies `fcitx5-remote -n`, retries briefly, does one short settle-check, then sends a final `fcitx5-remote -s tipe` before the last verification; it does not edit the fcitx5 profile. A model-enabled restart also checks the verified process's `TIPE_MODEL_COMMAND` through `/proc` when readable and fails if the expected command was lost. If `DBUS_SESSION_BUS_ADDRESS` is not available, it refuses to restart fcitx5 instead of starting a broken daemon outside the real user D-Bus/Wayland session.

Verbose cursor/candidate diagnostics are off by default. Enable them only for debugging:

```bash
./scripts/restart-fcitx5.sh --dry-run --debug
./scripts/restart-fcitx5.sh --debug
```

Without `--debug`, normal input does not append `tipeui.log`, `engine-trace.log`, or GTK fallback geometry rows. Existing files are retained as historical diagnostics, so their presence alone does not mean logging is currently active. With `--debug`, these user-only files are bounded: the first two retain roughly the newest 3 MiB after crossing 4 MiB, and the GTK fallback truncates its open log after crossing 4 MiB. `tipe-doctor` reports all three current sizes and the trim target.

The log-based checks below assume fcitx5 was deliberately restarted with `--debug`; return to a normal restart after collecting the needed trace.

On HiDPI Xwayland, the X11 root may use physical pixels while the GTK fallback uses logical coordinates. The pointer and Wine-caret fallbacks compare both desktop sizes and map their points explicitly even when a physical point in the left part of the screen also lies numerically inside the logical monitor bounds. Wine 11 may leave every XIM `XNSpotLocation` update at `0,0,0,0`; when the foreground X11 PID is a same-user Wine process, the installed `tipe-wine-caret-bridge.exe` instead asks that prefix for the standard MSAA `OBJID_CARET` location and whether the focused control has an IMM context. The bridge sleeps on a pipe between candidate updates, never reads text, and valid replies appear as `fallback<TAB>wine-caret` only during an explicit debug run. `tools/wine_caret_probe.cpp` is the verbose development probe for comparing UI Automation, GUI-thread, IMM, and MSAA returns; it is not started by normal TiPE input. If MSAA is absent too, exact arbitrary application-side motion remains unavailable and TiPE retains the stable X11-pointer fallback.

When a `wayland_v2` client reports `cursor=0,0,0,0` through fcitx, do not route that value to layer shell: the native `tipeui` popup relies on the compositor's input-method-v2 `text-rect`. Check `tipeui.log` for `popup	text-rect` and `popup	rendered`; `boundsOk=0` is a TiPE layout bug. After focus or visibility changes, `tipeui` invalidates the old rectangle and waits for a fresh callback. Status-only popups may reuse the last rectangle from the same popup.

Non-`wayland_v2` frontends use `tipeui`'s GTK status/candidate path. DBus physical coordinates are divided by the frontend scale before placement. XIM commonly reports a valid spot with zero dimensions; TiPE promotes that point to a logical caret instead of discarding it. If XIM reports `0,0,0,0`, the helper tries the on-demand Wine MSAA bridge first and otherwise captures the X11 pointer once per composition; snapshot metadata still records `pointerFallback=1` because both paths replace a missing protocol rectangle. The bridge's IMM-context result determines the Wine placement contract: a control with IMM support owns the rendered preedit and its MSAA caret is used directly, while a control without IMM support receives one deterministic Pango-measured advance from the MSAA insertion point. This avoids misclassifying fractional-scale accessibility jitter as client-rendered preedit motion. In the latter case, `tipe-wine-inline-preedit` draws the same pinyin at that insertion point in a transparent blue overlay; the window is non-focusable and its Wayland input region is empty, so it cannot take focus, keys, or clicks. Candidate clicks are accepted only when their snapshot serial and current text still match; `popup	candidate-click-ignored-stale` identifies a rejected stale event. `TIPE_WAYLAND_POPUP_EDGE_FALLBACK=1` remains diagnostic-only because compositor-local text rectangles must never become global layer-shell margins.

DBus toolkit clients may advertise `ClientSideInputPanel`, which normally makes fcitx send candidates back to the client for toolkit rendering. While `tipeui` is active, TiPE installs fcitx's custom panel callback for that capability and invokes the UI addon through `TipeUI::updateInputPanel`. During a debug run, `engine-trace.log` must then contain `ui-route direct frontend=dbus`, followed by an `update frontend=dbus` row in `tipeui.log`; seeing only the first row means the direct addon call did not reach the renderer. Client preedit is still delivered separately through `updatePreedit()`.

When TiPE is deactivated with an active preedit, `engine-trace.log` should show `preserve-state reason=deactivate` followed by `deactivate dropped-active-state-after-preserve`; activation within ten minutes should show `restore-state preedit=...`. The same input context is strong restore evidence, while a short metadata fallback can use matching program/frontend/display or surrounding text. The GTK fallback writes `fallback	position` rows to `candidate-window.log` with monitor geometry and `boundsOk`; those checks are meaningful only for a known global rectangle. The following placement variables are diagnostic-only:

After a real window/input-context switch, `popup\tinput-context-changed-recreate` means TiPE discarded the previous popup surface before accepting the new field's rectangle. Empty updates from an older context should no longer produce a later `popup\thidden` for the active popup.

```bash
TIPE_CANDIDATE_LEFT=64 TIPE_CANDIDATE_BOTTOM=156 ./scripts/restart-fcitx5.sh
```

`fcitx5-remote` and GUI configuration tools require a live user D-Bus session. In restricted shells, prefer `./scripts/smoke-test.sh` for non-invasive checks because it does not switch the active input method.
Use `./scripts/build.sh --sanitize` to regenerate `build-sanitize`, compile every TiPE C/C++ target with ASan/UBSan, and run the complete CTest suite with halt-on-error enabled. If the compiler's sanitizer linker scripts point to missing runtime libraries, configuration fails before compilation with a compiler-matched runtime diagnostic instead of leaving a partially built tree.
Use `./scripts/install.sh --dry-run` to preflight all build outputs, metadata, and helper scripts and list their selected `HOME/.local` destinations without creating or changing files. A real install first validates a complete private CMake staging tree, including CMake's unterminated final manifest row, and then atomically renames same-directory temporary files over live destinations; the smoke test injects an incomplete stage and verifies that existing live content remains untouched. Use `./scripts/uninstall.sh --dry-run` to inspect the managed removal set.
Use `./scripts/doctor.sh --no-runtime` for a read-only source-tree status report, or `tipe-doctor` after install. Without `--no-runtime`, it also asks `fcitx5-remote -n`, warns when the current input method is not `tipe`, checks for a running `fcitx5` process, and reports that process's TiPE model environment when `/proc/<pid>/environ` is readable. The `dictionary` section reports the preferred LibIME dictionary/language model, libpinyin fallback data, readable system Rime dictionaries, and base-vs-user learned first candidates for representative inputs such as `n`, `nihao`, `github`, `woc`, `shenglue`, and a natural long sentence. This separates missing backend data from polluted learning rows. When `busctl` is available, doctor first verifies that the user bus owns `org.fcitx.Fcitx5`; a restricted shell that cannot reach that bus gets the first-line preflight failure instead of launching an abort-prone `fcitx5-remote`. It does not restart fcitx5, switch input methods, edit profile files, or call model endpoints.
If smoke-test reports unmanaged legacy binaries such as `tipe-engine`, `tipe-gui`, `tipe-ctl`, or `tipe-rerank-test`, they are older TiPE prototype artifacts outside the current fcitx5 addon install path. They are reported for clarity but are not removed automatically.

For niri Wayland, verify the application environment includes fcitx5 modules where relevant:

```bash
env | rg 'WAYLAND_DISPLAY|GTK_IM_MODULE|QT_IM_MODULE|XMODIFIERS'
```

Recommended manual test targets are GTK text fields such as gnome-text-editor, Firefox, or other normal toolkit text entries. Alacritty is not a reliable first test target for Wayland input method behavior.

## Model Hook

Use the protocol example as a local stand-in for a real model wrapper:

```bash
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' | ./scripts/model-protocol-example.sh
printf 'protocol\t1\npreedit\twoc\ncandidates\t我操\t我曹\nsegment_chain\twoc\two\t我\tc\twocao\t我操\n' | ./scripts/model-protocol-example.sh
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\ncorrection_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\n' | ./scripts/model-adapter.sh
printf 'protocol\t1\npreedit\tihao\ncandidates\t以后\t一号\n' | TIPE_SUPERVISION_HISTORY=/tmp/tipe-supervision-history.tsv ./scripts/model-adapter.sh
printf 'protocol\t1\npreedit\tnihao\napplication\tAlacritty\ncandidates\t你好\t你号\nevents\tletter:n\tcursor-move:Down\ncontext\t刚才\t你好\n' | TIPE_MODEL_DRY_RUN=1 TIPE_MODEL_BACKEND=openai-compatible ./scripts/model-adapter.sh
TIPE_MODEL_DUMP_PATH=/tmp/tipe-model.tsv ./scripts/model-dump.sh < /tmp/tipe-model-request.tsv
./scripts/model-explain.sh /tmp/tipe-model.tsv
./scripts/model-explain.sh --panel /tmp/tipe-model.tsv
./scripts/learning-panel.sh /tmp/tipe-model.tsv
./scripts/learning-panel.sh --raw-panel /tmp/tipe-model.tsv
./scripts/learning-panel.sh --window /tmp/tipe-model.tsv
./scripts/learning-panel.sh --replay --check --command ./scripts/model-current.sh /tmp/tipe-model.tsv
./scripts/learning-panel.sh --replay --explain-output --command ./scripts/model-current.sh /tmp/tipe-model.tsv
./scripts/learning-panel.sh --replay --learn-output --command ./scripts/model-current.sh /tmp/tipe-model.tsv
./scripts/learning-panel.sh --replay --dry-run-model --command ./scripts/model-current.sh /tmp/tipe-model.tsv
./scripts/supervision-window.sh /tmp/tipe-model.tsv
./scripts/analyze-window.sh --learn-output --command ./scripts/model-current.sh /tmp/tipe-model.tsv
./scripts/model-replay.sh --request /tmp/tipe-model.tsv --check --explain
./scripts/model-replay.sh --request /tmp/tipe-model.tsv --explain-output
./scripts/model-replay.sh --request /tmp/tipe-model.tsv --learn-output
./scripts/model-replay.sh --request /tmp/tipe-model.tsv --dry-run-model --explain
./scripts/model-self-test.sh
./scripts/model-self-test.sh --current --config /tmp/tipe-model-env
./scripts/model-self-test.sh --adapter-dry-run
./scripts/model-config.sh --write heuristic --dry-run
./scripts/model-config.sh --write heuristic --dry-run --test
./scripts/model-config.sh --write heuristic --continuous on --dry-run
./scripts/check-preferences.sh --preedit nihao
./scripts/model-wrapper-new.sh --path /tmp/my-tipe-model-wrapper --configure --config /tmp/tipe-model-env --dry-run
printf 'protocol\t1\npreedit\tong\ncandidates\t弄\ncorrection\tihao\tnihao\t2\n' | TIPE_WRAPPER_DEBUG_SUMMARY=1 /tmp/my-tipe-model-wrapper
./scripts/model-wrapper-check.sh --command /tmp/my-tipe-model-wrapper
./scripts/model-config.sh --write custom --command "$HOME/.local/bin/my-tipe-model-wrapper" --dry-run
./scripts/model-config.sh --write llama-cpp --model "$HOME/.local/share/tipe/models/qwen2.5-1.5b-instruct-q4_k_m.gguf" --dry-run
./scripts/model-config.sh --write llama-cpp --model "$HOME/.local/share/tipe/models/qwen2.5-1.5b-instruct-q4_k_m.gguf" --dry-run --test-dry-run
./scripts/model-config.sh --write ollama --base-url http://127.0.0.1:11434/v1 --model qwen2.5:0.5b --dry-run
./scripts/model-config.sh --write ollama --base-url http://127.0.0.1:11434/v1 --model qwen2.5:0.5b --dry-run --test-dry-run
printf '%s\n' "$OPENAI_API_KEY" | ./scripts/model-config.sh --write openai --model your-openai-model-name --api-key-stdin
printf '%s\n' "$PROVIDER_API_KEY" | ./scripts/model-config.sh --write openai-compatible --base-url https://api.example.com/v1 --model your-provider-model --api-key-stdin
```

TiPE coalesces atomic replacement of the active request at `$XDG_CACHE_HOME/tipe/supervision-current.tsv` (or `~/.cache/tipe/`) to at most once per 250 ms, keeps the latest completed request in `supervision-last.tsv`, and appends sampled or terminal requests to a roughly 256 KiB `supervision-history.tsv`. The live current request may contain bounded surrounding text and recent committed context for an immediate clicked model call; a terminal boundary is written immediately and is never dropped by coalescing. Last/history persistence strips `surrounding_before`, `surrounding_after`, and `context` source text while retaining versioned `surrounding_features` and `context_features` fingerprints plus key trails, preedit, candidates, actual choices, correction evidence, and segment chains. A terminal snapshot is appended before commit or cancel clears composition. A normal post-commit reset preserves the bounded correction/commit session context for the next composition in the same input context; destroying the input context clears it.

Use `tipe-training-export --stats` to count supervised history samples without exposing their contents. New engines keep terminal actions in `supervision-training-history.tsv`, and the exporter prefers that file while falling back to `supervision-history.tsv` when needed. `tipe-training-export --limit 5` prints the newest labeled JSONL samples. The default excludes application names, the long correction trail, known evidence, and context features. Add `--include-context` or `--include-surrounding` to include only opaque fingerprints; add `--include-application`, `--include-correction-trail`, or `--include-evidence` only when those categories are intentionally needed. The model settings window exposes matching, default-off Chinese toggles for clicked personal-model training. The command reads history and writes only to stdout. `tipe-doctor` reports record counts and byte limits for both history files.

`tipe-personal-model-train` trains TiPE's click-triggered behavior model, **TiP**; it needs no model server or network access. The artifact combines hashed Chinese candidate ranking, a personal edit channel, hashed exact English-token memory, an independent `raw-offer` classifier, and a compact pinyin prior. Confirmed English-mode commits provide bounded auxiliary evidence, while separate validation gates control keyboard correction, unseen raw-English offers, and generic Chinese reordering. The training action distills repeated non-pinyin exact tokens plus validated positional correction patterns and global key habits into bounded runtime rows. Ordinary input can then apply those rules in C++ without rerunning TiP on each key; use `--no-runtime-distill` or an isolated `--preferences PATH` when needed. Distillation is a post-publication synchronization: it is retried three times, and failure leaves the newly published TiP artifact valid while the existing runtime rows remain active for a later retry. The command emits `runtime-distill<TAB>failed` but exits successfully in that partial-success case so the supervision window does not claim that model training failed. By default training reads Fedora Rime dictionaries when available. `tipe-personal-model inspect` reports the model name, feature version, exact-token counts, active patterns and habits, and all three safety gates independently.

The trainer defaults to `nice 10` when `nice` is available and exits after one run; it never installs a resident training daemon. Use `TIPE_PERSONAL_TRAIN_NICE=0..19` to change that explicit training priority while diagnosing throughput or desktop responsiveness.

Training output separates ranking readiness from component safety. `recommendation=collect-more-data` or `keep-heuristic` still means generic ranking is locked. A candidate with independently safe repeated keyboard correction plus a pinyin prior, or an independently validated raw-English profile, reports `component-update-safe=1` even when the generic holdout is below its first-candidate baseline. This permits `model-update-kind=safe-component-upgrade` to install the guarded component without unlocking unseen candidate reorder; max-count merging and capability-regression checks retain already active behavior from the existing artifact. A candidate with no independently safe component is still preserved. The supervision window reports these cases separately.

`tipe-supervision-window` is the main three-page view: `学习` shows collected/trainable/active counts, `模型` selects local or cloud backends and explains the request path, and `支持` displays the installed WeChat and Alipay codes. Opening it and pressing Refresh are read-only. `更新 TiP` uses accumulated terminal samples to update the local artifact without analyzing the active request. `分析当前输入` is a separate one-shot action for whichever TiP, local, or cloud backend is selected. A configured custom TiP path is used for both updating and analysis. Training completion reports holdout accuracy, first-candidate baseline, validation gain, non-leading sample count, and `model-updated`; when validation preserves the existing model, the window says so instead of implying that the new candidate became active. Use `tipe-learning-panel --window REQUEST_TSV` for a strictly read-only GTK view, or add replay options explicitly for lower-level diagnostics. Without an explicit request path, supervision, analysis, and replay helpers prefer the live snapshot, then the last snapshot, then the latest valid history request, then the configured dump path.
The quality section distinguishes ordinary holdout accuracy from `true generic` accuracy. Only non-leading samples with a preedit absent from training, no active preference, segment-chain, or correction-source support, and no exact-only derived prefix marker enter the generic denominator. The adjacent exclusion counters explain why an ordinary holdout sample was not a valid generic test; `排除派生前缀样本` still contributes to exact prefix learning and is excluded only from the generic claim.
`model-update-kind=safe-validation-upgrade` means a legacy evaluation artifact was replaced by a non-regressing evidence-isolated artifact while generic ranking stayed locked. It is a protocol cleanup, not a claim that unseen candidate ranking is ready.

History summaries count a candidate preference only when `events` confirms the same `candidate-selected` text or a matching `raw-committed` preedit. Merely highlighting a non-leading candidate is retained as UI context but is not learned. Prefix-only selections remain segment-chain evidence rather than full-preedit preferences. `tipe-model-explain`, `tipe-learning-panel`, generated wrappers, and HTTP adapter prompts expose the same active/pass-through counts, correction patterns, candidate metadata, and safe `suggested_protocol` versus known `evidence_protocol` distinction. Run `tipe-doctor --no-runtime` to inspect history health, or `tipe-model-replay --request PATH --check` to validate model output without changing the active input session.

For repeatable local/cloud selection, use `tipe-model-config` after install. It writes `~/.config/tipe/model-env`; a directly entered API key is stored separately in `~/.config/tipe/model-api-key` with mode `0600`, and only that path is written to `model-env`. `--api-key-env` remains available as an advanced alternative. The helper does not restart fcitx5, switch input methods, edit profile files, or call model endpoints. After a real write it prints the restart environment, self-test command, dry-run-test command, activation hint, and a note that no fcitx5 restart or input-method switch was performed. `tipe-model-config --show`, `tipe-model-current --show`, and `tipe-doctor --no-runtime` report the key source and set/unset state but never its value. `tipe-model-current --print-env` prints only the private key-file path or environment reference. For `llama-cpp`, diagnostics additionally report one-shot invocation, `llama-cli` executability, GGUF readability, CPU thread count, context size, and the 30-second default timeout. The `process-command-active` row only describes the helper process running the diagnostic, not the live fcitx5 runtime; run `tipe-doctor` without `--no-runtime` when `/proc` access is available to inspect the live model command and config. Endpoint base URLs must be `http` or `https`, and chat paths must start with `/`. `tipe-model-current` reads the config and dispatches to `tipe-model-adapter` or `tipe-model-dump`; `tipe-restart-fcitx5` loads it by default and propagates the selected model command and bounded continuous/timeout settings into fcitx5.

`llama-cpp` is not a background service. Each Analyze/F9 request creates private temporary prompt/output files, starts one `llama-cli` process with the configured GGUF, filters the result, removes those files, and exits. The defaults are CPU-only, 6 threads, and an 8192-token context; override them with `--llama-command`, `--llama-threads`, and `--llama-context`. This has zero idle model process cost but reloads the GGUF for each click. `--test-dry-run` validates the full JSON prompt without requiring `llama-cli` or the GGUF to exist.

The GTK window presents TiP, local-large-model, and cloud-large-model choices first. `高级设置` still exposes personal, llama.cpp, request-dump, custom, timeout, generation, and path values for debugging. Backend-specific fields round-trip the effective configuration instead of resetting hidden values on save. It writes through `tipe-model-config`; a directly entered API key travels over subprocess stdin to the private key file and never appears in argv. The local `检查填写内容` action adds `--dry-run`, so it validates form values without calling the selected endpoint. The next window analysis uses the saved config immediately; the live fcitx5 path still needs an intentional `tipe-restart-fcitx5` to reload the reusable model command.

Learning output written by that window does not require an fcitx5 restart. The engine checks the preference file before the next candidate refresh/rerank and before its own writes. To diagnose apparent stale learning, use `tipe-check-preferences --preedit PINYIN`, then type or edit the relevant preedit once; the active state should reload the same ranking, correction, and segment-chain rows. The main `学习` page intentionally shows only outcome counts, not raw evidence rows.

For a live TiPE session, start fcitx5 with the variable set before switching to TiPE:

```bash
./scripts/restart-fcitx5.sh --model-example
# or
./scripts/restart-fcitx5.sh --model-adapter
# or, after tipe-model-config --write ...
./scripts/restart-fcitx5.sh
```

Then `F9` while composing asks the model hook once. The hook may print `candidate<TAB>...` rows to reorder existing candidates, `correction<TAB>typo<TAB>corrected-preedit` rows to teach typo correction, `preference<TAB>preedit<TAB>candidate<TAB>count?` rows to explicitly persist a current-preedit preference, or `segment_chain<TAB>original-preedit<TAB>consumed-preedit<TAB>committed-text<TAB>remaining-preedit<TAB>corrected-full-preedit<TAB>combined-candidate<TAB>count?` rows to persist a confirmed segmentation/correction chain. Accepted candidate rows that promote a non-leading candidate are also saved as lightweight current-preedit preferences, so the same input can improve locally after one clicked model decision; echoing the current first candidate or an already stored candidate preference does not create another learned preference. Explicit preference, correction, and segment-chain rows that only echo learning records TiPE already sent in the request or already has in the preference file are not written back again, so a wrapper can inspect known evidence without accidentally increasing its count on every Analyze/replay. `Shift+F9` toggles continuous mode in the live engine, but continuous mode is deliberately local-only and will not call this model hook; with no active preedit the toggle briefly shows `Auto` or `Manual`, and `tipe-doctor` reports the latest `continuous-mode toggled=...` trace when observed.
Model calls time out after 2 seconds by default; `llama-cpp` config uses 30 seconds and the restart helper propagates it to the parent engine. Set `TIPE_MODEL_TIMEOUT_SECONDS=1..30` beside `TIPE_MODEL_COMMAND` when testing slow wrappers. Timeout cleanup terminates the wrapper's process group, including child HTTP/model processes.
TiPE runs the model command directly, not through a shell. Simple `KEY=value` prefixes and safe space-separated arguments are supported; use a wrapper script for pipes, quoted arguments, credentials, or complex launch logic.
The adapter itself defaults to offline `heuristic` mode. For one-shot local GGUF analysis, use `tipe-model-config --write llama-cpp --model /absolute/model.gguf`. For OpenAI or another compatible endpoint, open `选择和配置模型`, choose a cloud provider, fill the model, endpoint when needed, and API Key, then choose the two optional data categories. `检查填写内容` validates locally and never contacts the provider. Cloud configs default `TIPE_MODEL_SEND_RECENT_INPUT=0` and `TIPE_MODEL_SEND_SURROUNDING=0`, so the fixed `tipe.cloud-rerank.v1` prompt initially contains only the current composition/candidates and current candidate UI state. Enabling the first permission adds bounded recent keys, edits, history, and learning evidence; enabling the second adds cursor-surrounding text and the application name. The one-shot response still accepts only candidate/correction/preference/segment-chain rows and validates them locally.

Generated custom wrappers expose leading-key context as `preedit_leading_events`, `preedit_leading_event_counts`, and `preedit_leading_events_before_preedit`. With `TIPE_WRAPPER_DEBUG_SUMMARY=1`, they print matching `wrapper-debug	preedit-leading-*` rows so local scripts can inspect pass-through keys before the current preedit without reparsing the raw event trail.
