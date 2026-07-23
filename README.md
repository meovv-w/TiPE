# TiPE

TiPE is a clean fcitx5-first prototype for an AI-oriented Chinese pinyin input method. This restart intentionally does not reuse old IBus, GNOME Shell extension, Rust D-Bus, or fcitx5 prototype code.

The first milestone is a stable minimal input method for niri + fcitx5. It uses a pure C++20 state machine and, when available, the same LibIME pinyin dictionary and Chinese language model used by fcitx5-pinyin. Libpinyin remains a fallback backend. Installed Rime dictionaries contribute bounded exact words and explicit mixed/custom composition, while a minimal built-in fallback keeps basic input usable when system data is unavailable. Historical feedback sentences are not production dictionary entries.
The next layer is an input behavior model that records meaningful key actions, can learn correction patterns such as repeated missing-letter fixes, can prefer raw English-like text, persists lightweight candidate preferences, and exposes a click-style model hook. The hook is model-agnostic: TiPE can use its built-in lightweight behavior model, a user-provided local model wrapper, or a cloud model wrapper without hard-coding one provider.

## Build

Required build dependencies are CMake 3.20+, a C++20 compiler, fcitx5 development files, Wayland client headers and `wayland-scanner`, PangoCairo, GTK4, gtk4-layer-shell, and XCB. LibIMEPinyin is the preferred optional candidate backend. Libpinyin is an optional fallback; installed Rime dictionaries and TiPE's minimal fallback keep the project buildable without either library.
An `x86_64-w64-mingw32-g++` cross compiler enables the optional Wine caret bridge. Without it, TiPE still builds and uses the ordinary XIM pointer fallback; with it, CMake builds and installs the small on-demand MSAA helper used for exact caret placement in Wine applications that expose `OBJID_CARET`. The helper also reports only whether the focused control has an IMM context; it never returns composition or application text.
The TiP training helper uses `flock` from util-linux to ensure that two learning windows cannot publish competing model files at the same time.
Explicit TiP training runs at `nice 10` when the system `nice` command is available, so a training click yields CPU time to the desktop and fcitx5 input path. Set `TIPE_PERSONAL_TRAIN_NICE=0..19` only when a different one-shot training priority is intentional; this does not create a background service.

```bash
./scripts/build.sh
./scripts/smoke-test.sh
```

`build.sh` configures a `RelWithDebInfo` user-prefix build, compiles it, and runs CTest. It defaults to at most four build jobs at nice level 5 so development does not monopolize the desktop; set `TIPE_BUILD_JOBS` and `TIPE_BUILD_NICE=0..19` explicitly to override that policy. CTest covers the state machine and pass-through supervisor, training exporter, TiP, real localhost HTTP transport for OpenAI/Ollama-compatible backends, internal-mode toggle helper, the English supervision pipeline, candidate-window self-test, and learning-panel-window self-test; a clean unrestricted build should report 9/9 tests passed. The HTTP test returns CTest's skipped status only when a sandbox forbids even loopback sockets; it can then be run directly in a normal local shell without contacting the internet.
After the tests pass, it writes a SHA-256 fingerprint of every build input to `build/.tipe-build-tested`. When that fingerprint changes, the next build uses `--clean-first`, so clock rollback cannot make an older future-dated object look current. `install.sh` recomputes the fingerprint and refuses to install when the marker is missing or any project source/helper content changed, so an older binary cannot be mixed with newer scripts even if the system clock moves backward. Generated Python cache files are excluded. Source-tree helpers resolve sibling source files and `build/` tools before `~/.local/bin`; installed helpers resolve their peers in `~/.local/bin`.

Use `./scripts/build.sh --sanitize` for a clean Debug build with AddressSanitizer and UndefinedBehaviorSanitizer applied to all TiPE C/C++ targets. The sanitizer build directory is regenerated on each run so compiler upgrades cannot reuse stale runtime paths. CMake probes a minimal sanitized executable during configuration and stops with an explicit compiler-matched ASan/UBSan runtime message when the host toolchain is incomplete.

Inspect candidate ordering without switching the active input method:

```bash
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
TIPE_MODEL_COMMAND=./scripts/model-protocol-example.sh ./build/tipe-state-probe nihao --rerank
TIPE_MODEL_COMMAND=./scripts/model-dump.sh TIPE_MODEL_DUMP_PATH=/tmp/tipe-model.tsv ./build/tipe-state-probe nihao --application Alacritty --rerank
./scripts/doctor.sh --no-runtime
./scripts/check-preferences.sh /tmp/tipe-preferences.tsv
./scripts/check-preferences.sh --explain /tmp/tipe-preferences.tsv
./scripts/check-preferences.sh --summary --top 5 /tmp/tipe-preferences.tsv
```

`tipe-state-probe` uses a temporary preference file by default so probes do not change learned user ranking. Add `--preferences PATH` to reuse a specific test preference file across probe runs, or add `--user-data` when you intentionally want to inspect behavior with the real TiPE preference file.
Add `--learned-corrections` to print the bounded corrected-pinyin stream and dictionary priority produced by exact repairs and distilled TiP rules. This is the direct diagnostic for an input such as `woxiangyo` without running F9 or a model process.
Use `tipe-check-preferences --summary` on a preference file to see aggregate learned candidate, raw, and correction counts plus the strongest rows without modifying that file.

In TiPE's internal English mode, live supervision declares `supervision_state mode pass-through-only` and `runtime_state input_mode english`. The request contains only the current bounded raw token and edit trail, never arbitrary application text. Local and cloud model adapters receive the same mode marker, while model execution remains click-triggered.
Use `tipe-learning-panel REQUEST_TSV` for lower-level read-only diagnostics on a dumped model request without restarting fcitx5 or calling a model. During real composition, TiPE writes a current supervised request under `$XDG_CACHE_HOME/tipe/` and keeps a bounded last/history record after composition clears. `tipe-supervision-window` is the normal user entry point: its `学习` page separately shows the automatic LibIME word-order learner and click-updated TiP outcomes, its `模型` page selects and invokes a model, and its `支持` page shows the project WeChat and Alipay codes. Raw request fields and protocol evidence are not presented as the primary UI. `刷新数据` remains under `高级详情` and only reloads local state. `分析当前输入` runs the selected model once and persists only accepted safe rows. For a read-only replay window, use `tipe-learning-panel --window --replay --defer-replay --explain-output REQUEST_TSV` without `--learn-output`; add `--learn-output` only when clicked analysis should update the preference file. `tipe-analyze-window` is the shorter launcher for the same manual path.

The model page first presents three understandable choices: TiP local learning, a local large model, or a cloud large model. `选择和配置模型` then shows only fields relevant to that choice; developer backends, paths, timeouts, generation values, and TiP fingerprint options stay under `高级设置`. OpenAI needs a model name and API Key; another compatible provider also needs its API address. A directly entered key is sent to `tipe-model-config` over stdin and stored separately from `model-env` in a user-owned mode-`0600` file, never in subprocess arguments or diagnostic output. The two cloud data permissions default off. `检查填写内容` validates locally without contacting the provider or creating a bill. Saving updates the window immediately; the already-running fcitx5 process needs `tipe-restart-fcitx5` before its in-input analysis uses the new backend.
Probe output includes `visible<TAB>digit<TAB>candidate-index<TAB>text` rows, which show the current numbered row used by digit selection.
It also includes an `events` row with key-action counters, so model-input behavior can be checked without starting fcitx5.
Committed text that would be sent as model context is shown as `context<TAB>index<TAB>text` rows.
Add `--events` to include ordered recent event rows with escaped event text.
Add `--application NAME` to simulate the focused application name that TiPE passes to model backends during explicit rerank requests.
Add `--key KEY` to replay one engine-style key name such as `Down`, `KP_Down`, `ISO_Left_Tab`, `F9`, `KP_Enter`, or `KP_Add`.
Add `--continuous-mode` to mark the diagnostic UI state as continuous mode, and `--continuous-rerank` to simulate the continuous-mode lightweight rerank path; it keeps the candidate panel collapsed and does not call `TIPE_MODEL_COMMAND`.
`--digit N` follows real digit-key behavior: it selects a numbered candidate for normal pinyin and inserts the digit when it continues an active raw/alphanumeric token such as `qwen2`.
Add `--snapshot X,Y,W,H` to print the exact candidate-window stdin snapshot TiPE would send after the requested actions, including supervision metadata such as key counts, rerank count, continuous-mode state, and the pinyin edit cursor. This is useful for checking state-to-window protocol output without starting GTK or switching input methods.
For longer real-use reproductions, pass `--script FILE`. Each non-comment line is one action such as `type nihao`, `select 你`, `key KP_Down`, `move Down`, `move PageDown`, `move Home`, `digit 1`, `observe Tab`, `rerank`, `continuous-mode on`, `continuous-rerank`, `punct ,`, `space`, `enter`, `backspace`, `delete`, `restore-preedit nihao Delete`, or `escape`. `move` accepts `Down`, `Up`, `PageDown`, `PageUp`, `Home`, `End`, `Left`, `Right`, `Tab`, and `ShiftTab`, plus matching keypad and fcitx aliases such as `KP_Down`, `KP_Page_Down`, `Next`, `Prior`, and `ISO_Left_Tab`. Scripts can also assert state with `expect-preedit TEXT`, `expect-preedit-cursor INDEX`, `expect-candidate INDEX TEXT`, `expect-visible DIGIT CANDIDATE_INDEX TEXT`, `expect-event INDEX KIND [TEXT]`, `expect-has-candidate TEXT`, `expect-context INDEX TEXT`, `expect-model-row PREFIX`, `expect-no-model-row PREFIX`, `expect-expanded 0|1`, and `expect-selected INDEX`. The initial pinyin argument may be empty when the script itself starts with `type`.

```bash
./build/tipe-state-probe "" --script examples/probe-actions.txt
printf '%s\n' 'type nihao' 'select 你' 'expect-preedit hao' | ./build/tipe-state-probe "" --script -
printf '%s\n' 'type nihao' 'select 你' 'select 好' > /tmp/tipe-actions.txt
./build/tipe-state-probe "" --script /tmp/tipe-actions.txt
```

## Install For Current User

This installs TiPE files into `~/.local`, but it does not edit the fcitx5 profile or switch the current input method.

```bash
./scripts/install.sh --dry-run
./scripts/install.sh
```

`--dry-run` checks every required build artifact, addon metadata file, and executable helper before listing the complete managed destination set. It does not create `~/.local`, copy files, restart fcitx5, or switch input methods. A real install first sends the complete CMake output to a private staging directory, validates every staged file and rewritten addon metadata row, and only then replaces live files with same-directory atomic renames. A missing or malformed staged artifact therefore leaves the existing installation untouched, and a running fcitx5 process cannot open a partially overwritten shared library.

After you confirm that changing the current input session is desired, this helper restarts fcitx5 and attempts to switch the active input method to TiPE:

```bash
./scripts/restart-fcitx5.sh --dry-run
./scripts/restart-fcitx5.sh
fcitx5-remote -n
```

After TiPE is active, `tipe-toggle` switches TiPE's own Chinese/English mode without deactivating fcitx5:

```bash
tipe-toggle
tipe-toggle --status
tipe-toggle --set chinese
tipe-toggle --set english
```

The helper does not send a desktop notification. Successful mode changes are shown only by TiPE's own compact `TiPE`/`Eng` indicator.
With no arguments, it toggles the mode only when TiPE is already active. If fcitx5 is inactive or another input method is current, it activates TiPE in Chinese mode instead of applying a stale saved English mode; an explicit `--set english` still requests English mode.

The desktop shortcut must call this helper too. For niri, use this binding (inside `binds { ... }`):

```kdl
Mod+Space { spawn-sh "$HOME/.local/bin/tipe-toggle"; }
```

Do not bind the TiPE Chinese/English shortcut to `fcitx5-remote -t` or `fcitx5-remote -c`. Those commands deactivate the engine, so English keys bypass TiPE and cannot contribute supervised learning. `tipe-doctor` reports this integration error without modifying the niri configuration.

The mode file is `$XDG_RUNTIME_DIR/tipe/input-mode`, with the TiPE cache directory as a fallback. It is replaced atomically with user-only permissions and carries a unique request token. The helper first requires TiPE activation to remain stable, then waits for the engine to apply that exact token and atomically acknowledge it in `input-mode-applied`; seeing `fcitx5-remote -n` briefly report `tipe` is not treated as a completed switch. The engine installs its inotify watch before a final startup read, so a request cannot disappear in the read-before-watch gap. If Chinese preedit is active when English mode is selected, TiPE keeps that preedit visible and unchanged. Switching straight back to Chinese resumes it; the first printable English key commits the pending pinyin as raw text before that key passes through. Otherwise English mode passes keys to the application unchanged while supervising at most a 64-byte current token and a 256-event correction trail. Space, Enter, Tab, punctuation, leaving English mode, and input-context focus/lifecycle boundaries finish a known token; repeated identifiers become normal raw-English evidence, and delete/retype behavior becomes generic correction evidence. No local or cloud model runs for each key.

Supervision covers every key press delivered to the active fcitx5 input context. Key-release events are ignored except for debouncing the explicit model trigger. Compositor-reserved shortcuts are consumed before any input method can receive them; TiPE records its own mode switch and focus/lifecycle boundaries explicitly instead of claiming to capture those unavailable global key combinations.

Password fields are an explicit exception. When an input context advertises fcitx5's `Password`, `Sensitive`, or client-side `Disable` capability, TiPE immediately discards any composition, pass-through token, pending model request, and restorable state owned by that context. It does not read surrounding text, render a TiPE status/candidate popup, write current/last/history supervision, persist learning, or invoke a model; press and release events pass to the application unchanged. Capability changes are watched while the field remains focused, and the same check is repeated at every engine, snapshot, deferred-window, and model-result boundary. This protects frontends that allow an input method to remain nominally active in a private field.

Personal training treats these records in two channels. Multi-candidate records train pairwise ranking; one-candidate English records train correction patterns without inventing fake Chinese candidates. Model metadata reports `ranking-samples` and `correction-only-samples` separately.

If `fcitx5-remote -m tipe` prints `tipe` but `fcitx5-remote -s tipe` leaves the current input method unchanged, the addon is installed but the active fcitx5 profile does not include TiPE. Add `Name=tipe` to the current group in `~/.config/fcitx5/profile` only after you intentionally choose to change the current input session; stop fcitx5 before editing or the running process may write the old profile back on shutdown. Keep a backup of the profile before making this change.

`tipe-restart-fcitx5` (or `./scripts/restart-fcitx5.sh` in the source tree) is a current-user session helper and requires `HOME` to be set; use `--dry-run` first to see the planned command, launch mode, and corresponding log destination. A user-service launch writes to `journalctl --user -u fcitx5.service`; only the direct fallback writes `~/.cache/tipe/fcitx5.log` (or its `XDG_CACHE_HOME` equivalent), with user-only permissions. When a user `fcitx5.service` is available, the helper puts TiPE's transient model/debug variables into the user systemd manager, stops any old fcitx5 instance, and starts that service. It records the service's initial `MainPID`, requires the unit to remain active with exactly that one process before and after every activation/query step, and fails if a crash restart or D-Bus activation substitutes another fcitx5 process. This avoids racing a service restart with a second direct process, prevents a replacement daemon from being reported as a successful restart, and keeps the model environment across a service-managed run. A direct `nohup` launch remains the fallback when the service is unavailable or `--ui` requires custom arguments. After a real restart it asks fcitx5 to activate TiPE, verifies `fcitx5-remote -n`, retries a few times, performs one short settle-check, and sends a final `fcitx5-remote -s tipe`. It then waits through one more settle interval before reporting the final method, so a delayed fallback is warned about instead of being reported as stable TiPE activation. It warns when the active profile may not include TiPE. It does not edit the fcitx5 profile or service file. By default it loads `tipe-model-current`, prints the model config path and whether it exists in dry-run mode, passes `TIPE_MODEL_COMMAND`, the exact `TIPE_MODEL_CONFIG` path, and configured `TIPE_CONTINUOUS_MODE` and `TIPE_MODEL_TIMEOUT_SECONDS` values into the fcitx5 process, and prints a `tipe-doctor` verification hint. `--model-current` remains an explicit compatible spelling; `--no-model` is the intentional opt-out. A real model-enabled restart verifies `/proc` when readable and fails if the final process did not inherit the expected model command. It refuses to restart fcitx5 when `DBUS_SESSION_BUS_ADDRESS` is unavailable, because restricted shells can start a broken daemon without the real user D-Bus/Wayland session and can make `fcitx5-remote` abort instead of returning a normal error.
The install prefix is `~/.local`. Existing system input methods such as ibus-libpinyin, fcitx5-pinyin, and fcitx5-rime are not modified.
The fcitx5 engine addon metadata is installed as `~/.local/share/fcitx5/addon/tipe.conf`, the TiPE UI addon metadata as `~/.local/share/fcitx5/addon/tipeui.conf`, the input method metadata as `~/.local/share/fcitx5/inputmethod/tipe.conf`, and the modules under both `~/.local/lib64/fcitx5/` and `~/.local/lib/fcitx5/`. The install script also refreshes compatibility copies at `~/.local/share/fcitx5/addon/`, plus helper commands such as `tipe-candidate-window`, `tipe-model-adapter`, `tipe-model-explain`, `tipe-learning-panel`, `tipe-supervision-window`, `tipe-analyze-window`, `tipe-training-export`, `tipe-model-config`, `tipe-check-user-dictionary`, and `tipe-restart-fcitx5`. The source addon metadata keeps `Library=libtipe` and `Library=libtipeui`; the user-installed metadata points to addon-directory compatibility copies without the `.so` suffix because fcitx5 appends the platform suffix while resolving shared-library addons.

The install also adds `TiPE` to the desktop application menu through `~/.local/share/applications/tipe-supervision.desktop`, with the TiPE geometric T icon. The window has three primary pages: `学习` reports collected, trainable, and active learning counts; `模型` selects TiP, a local model, or a cloud API and shows the request flow; `支持` displays scan-ready WeChat and Alipay codes. Opening or refreshing the window does not invoke a model.

Use `tipe-doctor` after install, or `./scripts/doctor.sh` from the source tree, for a read-only status report. It prints TiPE install files, data files, effective model-mode summary rows, recent logs, and runtime status when available. Runtime mode also reports the running fcitx5 process's TiPE model environment when `/proc/<pid>/environ` is readable, including whether the process is using `tipe-model-current` and whether its `TIPE_MODEL_CONFIG` path matches the config path being inspected. Before invoking `fcitx5-remote`, it uses a read-only user-bus status query when `busctl` is available; an unreachable restricted-session D-Bus is reported without launching the abort-prone remote helper. It does not restart fcitx5, switch input methods, edit profile files, or call model endpoints:

```bash
tipe-doctor
tipe-doctor --no-runtime
```

## Uninstall

Preview the TiPE-managed user install files without deleting them. Each row is reported as `present<TAB>PATH` or `missing<TAB>PATH`:

```bash
./scripts/uninstall.sh --dry-run
```

Remove those files:

Before removal, replace any niri shortcut that calls `tipe-toggle`; the uninstall preview reports a warning when it detects that binding but never edits niri itself.

```bash
./scripts/uninstall.sh
```

Restart fcitx5 after uninstalling only when you are ready to change the current input session:

```bash
fcitx5 -r
```

## Current Behavior

Normal typing keeps every delivered key press relevant to supervision in a bounded in-memory trail without continuously running a model. TiPE coalesces active-request serialization and disk replacement to at most once per 250 ms, samples non-terminal general history at most once per 2 seconds, writes `supervision-last.tsv` only when a request completes, and retains every terminal training record. A terminal boundary flushes immediately, so coalescing does not lose the final correction or selection trail. The supervision window caches heavier history/model metadata while input is changing.

Long pinyin and mixed Chinese/English input use first-letter-indexed syllable matching, and mixed known tokens such as `github` bypass libpinyin's inapplicable bigram lookup. This keeps candidate generation bounded without removing the mixed-token composition path.

- In Chinese mode, letters enter preedit. Switching to internal English mode freezes any active preedit until either Chinese mode resumes or the first printable English key commits that preedit as raw text; ordinary English keys then pass through unchanged while TiPE records bounded supervision for later local or clicked-model learning.
- `Backspace` deletes preedit.
- `Delete`, navigation keys, and pass-through control keys are observed so local learning and clicked models can use correction/navigation behavior without blocking normal application input.
- Prefix pinyin such as `ni` can show candidates before the full key `nihao` is typed.
- A complete or still-unfinished single pinyin syllable prefers single-character candidates over unrelated backend phrase completions. For example, `s` starts with `是` instead of a longer phrase such as `省略`, while the longer phrase remains available later in the candidate list.
- Selecting a prefix candidate from a longer preedit commits that prefix and keeps the remaining pinyin, including multi-character prefixes such as `jixuzuo -> 继续 + zuo`, known typo/custom prefixes such as `haodewokanyxia -> 好的我看一下 + haiyoumeiyu`, and mixed English-token prefixes such as `dagithubdeshihou -> 打github + deshihou`. Multi-character prefixes that lead the current best full-sentence candidate are kept ahead of single-character fallbacks, so long input can be committed in useful chunks without losing the remaining pinyin. Ambiguous splits such as `nanizuobei` prefer the prefix that actually produced the chosen candidate, so selecting `那` leaves `nizuobei`.
- Long-sentence divergence filtering compares candidates with both exact dictionary prefixes and the active decoder's bounded prefix results. A traditional Rime prefix therefore cannot make a coherent simplified LibIME sentence look unrelated and push it behind every short prefix candidate. When LibIME's exact full-pinyin sentence has a decisive score lead, TiPE preserves that order instead of letting a weaker dictionary prefix replace it.
- Recent committed text can promote natural continuations while the remaining pinyin is still active. For example, after selecting `你` from `nihao`, the remaining `hao` prefers `好` instead of an awkward backend ordering such as `号`.
- `Space` commits the highlighted candidate, or raw pinyin when there is no candidate.
- ASCII punctuation such as comma and period commits the current candidate first, while leaving the punctuation key for the application to receive.
- Common developer English tokens such as `tipe`, `git`, `github`, `docker`, `npm`, `node`, `cargo`, `cmake`, `python`, `chatgpt`, `niri`, `ollama`, `qwen2`, `gpt4`, and `ipv6` are shown as the first candidate and commit as raw text, avoiding awkward transliteration while the input method is still active. The same tokens can compose with Chinese context, such as `chatgptdeshihou -> chatgpt的时候`, `woxiangyongdocker -> 我想用docker`, `woxiangyongqwen2 -> 我想用qwen2`, and `dakaigit -> 打开git`; the raw token can still be selected first while leaving suffix pinyin active.
- Other English-like identifiers are also surfaced as an explicit raw candidate when they match the shared English-shape classifier, even before TiPE has learned them. For example, `start` or `goal` can stay behind the leading Chinese candidate at first while still appearing in the visible row for one-step selection; after one explicit raw selection or repeated raw commits, the learned raw-English preference moves it to the front. TiPE suppresses this provisional raw candidate while the current letters can still complete a pinyin sequence, so intermediate input such as `shuruf` or `zenm` does not flash an English choice before `shurufa` or `zenme` is finished.
- Repeated raw commits teach raw-English preference for inputs that look like English identifiers, even when the token is not hard-coded. The generic classifier recognizes common non-pinyin consonant endings and English letter patterns, including short tokens, while preserving ordinary pinyin and `lv`/`nv` forms. Ordinary pinyin committed with `Enter` remains in bounded supervision history for clicked analysis but is not persisted or sent to a model as raw-English preference. A single identifier commit is not enough to override normal Chinese candidates, and later repeated Chinese candidate selections can push the ordering back.
- An uppercase letter typed during an active lowercase composition switches that composition to case-preserving raw English instead of committing a Chinese candidate. For example, `nihao` followed by `Shift+A` remains visible as `nihaoA`; later lowercase letters continue the same raw token and `Space` commits it once. With no active composition, an uppercase letter still passes directly to the application while remaining available to bounded supervision.
- Candidate display is collapsed to one horizontal row by default. Both collapsed and expanded layouts use stable visual cells; longer candidates occupy as many cells as their text needs, and later short candidates backfill earlier row gaps instead of leaving avoidable blank slots. `Down` expands the same candidate stream into a six-column grid; `Down/Up` move by row, `Left/Right` move within the grid, and moving past either end wraps around.
- On both the TiPE Wayland popup and its GTK frontend fallback, moving the pointer over a candidate highlights its visual cell and a left click selects it through the same prefix-preserving, supervision, and learning path as keyboard selection.
- `Tab`/`Shift+Tab` move the highlighted candidate right/left, and `PageDown`/`PageUp` move by one six-column row.
- `Home`/`End` jump to the first/last candidate.
- `F9` triggers the configured model/rerank hook on demand. `Shift+F9` toggles continuous light rerank for the current fcitx5 process; it uses TiPE's in-process learning path and does not call external model commands on every key. TiPE deliberately does not bind `Ctrl+Space`, because fcitx5 normally reserves it for globally disabling and enabling the active input method. To start TiPE with continuous mode already on, write a model config with `tipe-model-config --write heuristic --continuous on` and restart through `tipe-restart-fcitx5` when you intentionally want to reload fcitx5.
- Keypad navigation keys follow the same candidate navigation rules, and keypad punctuation commits the current candidate before passing the punctuation through.
- `F9` requests candidate reranking while composing. With a configured `TIPE_MODEL_COMMAND`, TiPE immediately expands the unchanged candidate order as feedback and runs the command once on a worker; only the final model order is rendered, so a fast or slow local/cloud result cannot cause an interim order to flash and revert. Without a configured command, the built-in lightweight rerank is applied immediately. Rapid repeated `F9` events are debounced. Model output is applied only if the preedit, candidate list, highlight, and expansion state still match the request; typing, navigating, selecting, clearing, or changing input context makes the old result stale and it is discarded. One candidate per output line remains the compatibility ordering format, and `TIPE_RERANK_COMMAND` is still accepted as the old compatibility name.
- External model output can persist explicit preferences, typo corrections, and segment-selection chains, but candidate or learning rows that only echo evidence already present in the current request or preference file are not written back again. This keeps Analyze/model replay from inflating old counts just because TiPE supplied them as context.
- Model requests expose active candidate preferences only when the candidate still exists for the current preedit, consumes the full preedit, passes the same shape checks as local ranking, and has reached its activation threshold. Old rows produced by earlier prefix-selection bugs remain inspectable but cannot be amplified by clicked AI analysis.
- `Shift+F9` toggles continuous mode. In this mode TiPE keeps supervising the same bounded key stream but only runs the built-in lightweight rerank when the composing text changes; it keeps the panel collapsed and shows a small blue indicator in the candidate window. If no preedit is active when the toggle is pressed, TiPE briefly shows `Auto` when continuous mode turns on and `Manual` when it turns off. Continuous mode intentionally does not call `TIPE_MODEL_COMMAND`, so local large models and cloud wrappers remain click-triggered through `F9`.
- The built-in ranking path can also try bounded local typo repairs without running a background model. For example, obvious noisy full-preedit typos such as `jibengongneg` can rank the corrected `jibengongneng -> 基本功能` candidate first while suppressing noisy segmentation candidates from the visible row. The broader repair search is bounded and conservative, so normal pinyin itself is not displaced by speculative longer variants; `F9` can still ask the configured model hook for an explicit rerank.
- Unknown English-like text is not promoted just because the current app or surrounding text looks like code. If the user repeatedly commits a raw identifier such as `started`, TiPE learns that preference and can put `started` first everywhere; normal pinyin-shaped input such as `shurufa` stays Chinese unless the user explicitly teaches otherwise.
- When the active first candidate is the raw English preedit, digit keys and common token symbols extend that raw token instead of selecting a numbered candidate or committing punctuation. This lets development tokens continue naturally, such as `react` + `1` -> `react1`, `react` + `-` + `a` -> `react-a`, or `started` + `F9` + `1` -> `started1`. Chinese candidates keep the normal number-selection and punctuation behavior.
- Explicit candidate choices are remembered under the user's TiPE data directory and affect later ordering; moving to another candidate, choosing a numbered candidate, clicking a candidate, or accepting model learning is strong evidence. Merely pressing `Space` or punctuation on the already-leading default records supervision but does not persist a ranking preference. Prefix selections from a longer preedit train the actual prefix pinyin, not the whole long preedit, so choosing `继续` from `jixuzuo` does not make `继续` outrank the full `继续做` candidate next time.
- A prefix choice followed by a suffix choice records one segment chain. One chain is not permanently published, but may be offered as a staged candidate when the prefix already has active selection evidence; after the same full chain is confirmed twice, its combined text becomes a protected whole-preedit candidate during ordinary typing. This lets repeated choices such as `lian + zhao -> 连招` affect the next `lianzhao` without running F9 or a background model.
- Undoing a partial candidate commit restores the full preedit and removes the undone committed text from the live model context, so supervision does not keep treating deleted text as current surrounding content.
- Repeated correction patterns are learned locally from the key event stream. If the user types a typo, deletes it with `Backspace` or cursor-positioned `Delete`, then types and commits a nearby corrected preedit, TiPE can later let the typo borrow the corrected preedit's candidates. Partial rewrites are also tracked, and explicit in-preedit edits such as moving the pinyin cursor left to insert a missing letter or pressing `Delete` on an extra middle letter teach the same kind of typo-to-preedit repair after repeated observations. Repeated adjacent transpositions such as final `gn -> ng` also become position-aware realtime corrections for pinyin up to 16 letters, without invoking the external model. One observation stays inactive, English-like identifiers remain protected, and a generalized transpose only leads when the current long candidate stream is a noisy invalid-pinyin segmentation.
- Exact typo learning requires a unique strongest correction before it changes candidates automatically. Competing corrections with equal evidence stay visible to clicked analysis but do not make the realtime path guess; once one correction has stronger repeated evidence, only that correction is applied automatically.
- Number keys select candidates in the currently numbered row. In expanded mode, moving to another row moves the visible number labels to that row, matching the macOS-style interaction. A digit is inserted into preedit when it continues a known alphanumeric developer token such as `qwen2`, `gpt4`, or `ipv6`, or when the current first candidate is the raw English preedit. Otherwise it remains candidate selection. In raw English mode, `-`, `_`, `.`, and `/` also extend the current token; outside that mode punctuation commits the current candidate first and passes through to the application. Digits without a visible candidate number, including empty slots in a partial final row, commit the current candidate first and then pass through to the application.
- In expanded mode, `Left`/`Right` and `Shift+Tab`/`Tab` follow the visible row-and-column order. When a long candidate moves to the next row and later short candidates backfill the first row, horizontal navigation stays on the visible row instead of jumping to the long candidate's array position.
- `Enter` commits raw pinyin.
- `Esc` clears preedit.
- Candidate cursor movement while composing is consumed and recorded as current ranking and training evidence.
- Release events are ignored.

Candidate lookup uses the user dictionary first, then LibIME's pinyin decoder and `zh_CN` language model when available. LibIME uses three whole-sentence paths, a 15-word candidate limit, and a four-character partial-long-word threshold. This stays close to fcitx5-pinyin's mature defaults while retaining one extra ambiguous segmentation for TiPE's prefix-selection flow; exact input such as `fenxiangyixia` is no longer buried under dozens of fuzzy sentence completions. Exact full-pinyin candidates with a clear language-model margin are protected from TiPE's later prefix heuristics, while near-tied homophones remain eligible for prefix disambiguation. LibIME is not invoked for an unsegmentable input longer than eight ASCII letters: its ambiguous graph can otherwise block the key thread for seconds after a missing or transposed key. Those inputs use the bounded system-dictionary and learned-correction paths until they become valid again; complete long pinyin and short typo completion still use LibIME. Libpinyin is used when LibIME cannot serve the request. Normal Rime contribution is bounded to high-weight exact words so it cannot append a large rare-character tail or mechanically outrank the language model; mixed English input, explicit `TIPE_SYSTEM_RIME_DICTIONARY` overrides, and no-LibIME fallback can still use Rime phrase composition. The Rime loader compacts spaced pinyin such as `sheng lue` to `shenglue`. Raw English tokens use the shared token model rather than transliteration entries in the Chinese fallback. Repeated raw-English commits are exposed to clicked/local/cloud model prompts as `behavior_summary.raw_english_hint` with source `learned-raw-preference`, so wrappers can use generic learned evidence without hard-coding a word.

## User Dictionary

TiPE can load and learn a private user dictionary before LibIME/libpinyin/fallback ordering. The default path is `~/.local/share/tipe/user-dictionary.tsv`, or set `TIPE_USER_DICTIONARY` to test another file. TiPE checks the file timestamp during lookup and reloads it after edits. Each non-comment line is:

```text
pinyin	candidate1	candidate2
```

The pinyin field must be lowercase `a-z`. Pinyin is limited to 128 bytes, candidates to 256 bytes, each row to 32 candidates, and the file to 4096 entry rows. Empty, malformed, oversized, and duplicate candidates on the same line are rejected by the checker and ignored by the runtime loader.

Example:

```text
nihao	你号	你好啊
zidingyi	自定义
```

The repository also includes `examples/user-dictionary.tsv`, which is safe to inspect or validate without changing the real user dictionary:

```bash
./scripts/check-user-dictionary.sh examples/user-dictionary.tsv
./build/tipe-state-probe nihao --dictionary examples/user-dictionary.tsv
```

Validate the file after editing:

```bash
./scripts/check-user-dictionary.sh
./scripts/check-user-dictionary.sh --explain
# or, after install:
tipe-check-user-dictionary
tipe-check-user-dictionary --explain
```

`--explain` prints each valid row's candidate order, which is the order TiPE tries before LibIME, libpinyin, installed Rime dictionaries, and fallback candidates.
Use `--add PINYIN CANDIDATE...` to safely create or update one row. Existing candidates are kept in order, new candidates are appended once, and the final file is validated before replacing the original. Add `--first` when you want the given candidates to become the first choices for that pinyin; existing duplicates are moved instead of copied. Engine learning and this helper share the same lock, mode-`0600` files, and atomic replacement, so concurrent updates cannot overwrite one another:

```bash
./scripts/check-user-dictionary.sh --add nihao 你好 你号
./scripts/check-user-dictionary.sh --add nihao 你好 --first
./scripts/check-user-dictionary.sh --add dgithubdeshihou 打github的时候 --path /tmp/tipe-user-dictionary.tsv
```

When a phrase is absent, the user can select it in two or more pieces, including character by character. TiPE accumulates the complete multi-step chain instead of forgetting earlier pieces. The first identical completed chain is staged as evidence; when its prefix choice already has active supporting evidence, that staged phrase can be offered immediately without permanently writing a new word. The second confirmation promotes the combined non-ASCII phrase into the user dictionary under its corrected full pinyin. It is then an immediate first candidate in the current process and after restart, without F9, continuous mode, or a cloud call. This threshold prevents one accidental segmented selection from permanently polluting the dictionary. Selecting a complete candidate that already exists continues to use the lighter candidate-preference path instead of copying ordinary system words into the user dictionary.

## Learned Preferences

Normal Chinese candidate commits also update LibIME's own local user language model. This is the same mature history-bigram mechanism used by fcitx5-pinyin, so ordinary words and sentence transitions improve without copying system dictionary entries or running TiP, Python, a daemon, or a cloud model. TiPE keeps its history separately at `~/.local/share/tipe/libime/user.history`, or under `$XDG_DATA_HOME/tipe/libime/`; it never writes fcitx5-pinyin's history. The file and lock are current-user-only, bounded to 4 MiB, and replaced atomically after each accepted LibIME candidate. The supervision window reports this layer as `正在工作`, `等待第一次中文选词`, or `已关闭`; it does not require the `更新 TiP` button. Set `TIPE_LIBIME_USER_HISTORY` to an alternate path, or to an empty value to disable this layer. `tipe-state-probe` uses a clean system language model by default and includes this user history only with `--user-data`.

TiPE separately stores lightweight explicit candidate preferences, typo corrections, and confirmed segment-selection chains in `~/.local/share/tipe/candidate-preferences.tsv`, or under `$XDG_DATA_HOME/tipe/` when `XDG_DATA_HOME` is set. Use `tipe-state-probe --preferences PATH` for isolated tests, and `check-preferences.sh` to inspect or validate a preference file without changing it:

Default candidate commits train the local LibIME history and supervision trail but are not added as explicit ranking preferences. Explicit selections use a strong weight and, when correcting an older choice for the same pinyin, immediately raise the selected candidate above its strongest competitor. Candidate preference counts are therefore bounded learning strengths rather than literal lifetime selection totals. Clicked/model learning uses validated preference rows, and old one-count candidate rows are retained for inspection but do not override the base language-model order or enter model requests as active preferences. Candidate ranking and exact local typo correction activate at count 2; raw-English preference activates at count 3. One-count corrections remain available as evidence for a clicked model to evaluate, but the local candidate path does not apply them automatically. Any explicit Chinese selection evidence for the current spelling blocks speculative global keyboard-habit correction, so a broad habit cannot override what the user has already demonstrated as intentional input. `tipe-check-preferences --explain`, `--summary`, and `--preedit` label lower candidate counts as `inactive-evidence` instead of reporting a next-ranking effect.

The running engine checks this file for changes before candidate refreshes, explicit reranks, and preference writes. Learning persisted by the supervision/analysis window therefore affects the next relevant input without rebuilding the input context or restarting fcitx5, and a later in-process selection first reloads external changes so it cannot overwrite newly learned rows with stale memory. Candidate metadata, rather than text-prefix similarity, decides whether a candidate consumes only part of the preedit; a complete word such as `你好` can still be learned even when a longer candidate starts with the same text. An active, full-preedit Chinese preference whose pinyin syllables align with its characters is also reinserted when the current decoder temporarily omits that learned word; partial selections and malformed/noisy history are not injected this way.

```bash
./scripts/check-preferences.sh
./scripts/check-preferences.sh --explain
./scripts/check-preferences.sh --preedit nihao
./scripts/check-preferences.sh --query-only --preedit nihao
./scripts/check-preferences.sh /tmp/tipe-preferences.tsv
```

`--query-only` is the low-latency path used by the live supervision window. It validates and reports only rows relevant to the requested preedit; omit it for a full-file audit.

During real composition TiPE also keeps a bounded recent supervision history at
`$XDG_CACHE_HOME/tipe/supervision-history.tsv`, or
`~/.cache/tipe/supervision-history.tsv` when `XDG_CACHE_HOME` is unset. Each record starts with a `---` header
containing the time, focused program, current preedit, candidate count, and
expanded/collapsed state, followed by a persistent form of the model request
protocol. Before `supervision-last.tsv` or history is written, TiPE removes
`surrounding_before`, `surrounding_after`, and cross-commit `context` rows;
the active `supervision-current.tsv` keeps them only while the current request
is live. Current and last snapshots are replaced atomically so the supervision
window cannot read a half-written request. The history file is trimmed automatically to roughly 256 KiB, so it
can feed click-triggered analysis without running a background model forever.
Repeated UI refreshes with an identical supervised request are not appended, so
cursor/panel redraws do not crowd out real key and candidate changes.

Terminal choices are also appended once to `supervision-training-history.tsv` in the same cache directory. This second history contains only candidate selections, raw commits, and cancels, is bounded to roughly 1 MiB by whole-record trimming, and preserves repeated choices as frequency evidence. It is written only on a terminal action, not on every key. `tipe-training-export` prefers this terminal history when it exists and falls back to the general supervision history on older installations.

Convert that bounded history into JSONL for local model training or offline evaluation with `tipe-training-export`. The default export includes only records with an explicit candidate selection, raw-text commit, or cancel target. The terminal action is removed from the input event list and written separately under `target`, so a trainer does not receive the answer as an input feature. Application names, the longer cross-composition correction trail, and persisted learning evidence are private opt-ins:

```bash
tipe-training-export --stats
tipe-training-export --limit 20 > /tmp/tipe-training.jsonl
tipe-training-export --include-correction-trail --include-evidence > /tmp/tipe-training-full.jsonl
tipe-training-export --include-application --all > /tmp/tipe-training-observations.jsonl
```

The exporter is read-only, uses only Python's standard library, does not call a model, and does not upload or alter supervision history or learned preferences. Each sample uses schema `tipe.training.v1` and keeps candidate metadata such as `consumed_prefix`, allowing a trainer to distinguish a full commit from a prefix choice that leaves pinyin active. Repeated equivalent choices are preserved because repetition is preference evidence; add `--deduplicate` for evaluation sets that need unique examples. `--all` additionally includes unlabeled intermediate observations.
When evidence export is enabled, a preference or completed segment-chain row that exactly matches the terminal target is removed from that sample's input. Repeated choices remain separate labels and pair-count evidence, but the preference written by the action being predicted cannot leak the answer back into its own model input.

When there is no active `supervision-current.tsv` and no readable
`supervision-last.tsv`, `tipe-supervision-window`, `tipe-analyze-window` through
`tipe-learning-panel`, and `tipe-model-replay` can extract the latest complete
request from this history and replay it as read-only supervision context. The
`tipe-doctor` report includes read-only health rows for this history file:
byte size, trim limit, modification time, record count, valid protocol request
count, active/pass-through record counts, and the latest header preedit/program.
It warns when the file looks malformed or contains records without complete
`protocol	1` requests. The advanced details and command-line diagnostics can
also summarize recent history records, including
common preedits, selected candidates, preedit-to-selected-candidate pairs,
applications, key-event counts split by active composition vs pass-through
keyboard context, correction event counts, repeated supervised
typo-to-corrected-preedit pairs reconstructed from correction trails, and
segment-chain totals. Local model requests can include that bounded summary. A
cloud request omits it by default and includes it only after the user enables
`发送近期按键和修改记录`; cursor-surrounding text and the application name have
a separate, also-default-off permission. When enabled, the summary includes
split `active_event_counts` and `pass_through_event_counts` so a model can
distinguish keys typed during Chinese composition from pass-through keys
observed while no preedit is active. `tipe-model-explain` prints the same
split as `behavior_history_active_event_count` and
`behavior_history_pass_through_event_count` when the bounded history is readable.
After a full commit or cancel, TiPE starts a fresh short `events` trail for the
next empty-preedit pass-through key or the next new preedit, so a later
pass-through-only request does not inherit the previous composition's letters or
candidate-selection event. The longer in-memory `correction_events` trail and
recent committed context are still retained for typo/correction learning.
The offline heuristic adapter also uses
repeated current-preedit history pairs conservatively: after the same preedit
has selected the same candidate at least twice recently, it can promote that
candidate or raw English preedit without a word-specific rule. Passive default
candidate-0 highlights are not counted as selected-candidate history; the
history signal requires a non-leading highlighted candidate or an explicit raw
preedit choice. Repeated
delete/retype, partial-rewrite, or middle-edit correction trails are also
summarized: after the same plausible typo correction appears at least twice
recently, the heuristic adapter can emit `correction<TAB>typo<TAB>corrected`
when either side of that repair matches the current preedit, and
OpenAI-compatible/Ollama/llama.cpp dry-run JSON exposes the same evidence as
`recent_history_summary.top_corrections` plus a
`history_correction` supervised learning signal.
When one input string ranks oddly, `tipe-check-preferences --preedit TEXT`
prints only the candidate preferences, corrections, and segment chains that can
explain that preedit, followed by `query-effect` rows that name the direct next
effect and a zero/nonzero query summary. Those effects distinguish candidate
ranking boosts, raw-English preferences, typo corrections that borrow candidates
from a corrected preedit, and segment-chain suffix continuation after a prefix
selection.

Preference rows are shaped as `preedit<TAB>candidate<TAB>count`; legacy rows shaped as `key<TAB>count` are accepted for compatibility. Confirmed English-mode exact tokens use the separate `__raw_token__<TAB>token<TAB>count` form, so trainer-validated non-pinyin words do not require broadening the ordinary raw-identifier classifier or activating old raw-pinyin rows. Correction rows are shaped as `__correction__<TAB>typo<TAB>corrected-preedit<TAB>count`. Clicked TiP training publishes validated positional rules as `__correction_pattern__<TAB>kind<TAB>typed<TAB>replacement<TAB>position<TAB>relative-to-end<TAB>count` and global keyboard habits as `__key_habit__<TAB>kind<TAB>typed<TAB>replacement<TAB>count`; an empty `typed` field represents a missing key. Position rules activate at 2 observations for missing/transposed keys, 3 for extra keys, and 4 for replacements. Broader position-independent habits require 3 observations for transposition, 5 for a missing key, and 6 for extra/replacement keys. Segment-chain rows are shaped as `__segment_chain__<TAB>original-preedit<TAB>consumed-preedit<TAB>committed-text<TAB>remaining-preedit<TAB>corrected-full-preedit<TAB>combined-candidate<TAB>count`. If `corrected-full-preedit` differs from `original-preedit`, TiPE treats that as an implied typo repair and accepts it only when the correction is plausible. A learned segment chain can also rerank the remaining suffix after a later prefix selection, for example a learned `nihao = 你 + 号` chain can make `号` lead after the user selects `你` and leaves `hao`.

The preference file keeps up to 2048 candidate preferences, 512 supervised English tokens, 512 corrections, 512 published correction patterns, 128 published key habits, and 512 segment chains. The engine applies the same limits in memory immediately after loading or merging the file, and equal-count rows use a stable key order, so a long-running fcitx5 process cannot grow beyond the persisted bounds or choose a different retained set after restart. Every stored count is limited to 1,000,000; larger values are rejected by the engine, training export, replay learner, and `tipe-check-preferences` so malformed or overflowed evidence cannot dominate ranking. Engine writes and clicked-analysis writes preserve the model-published runtime rules and use the same lock plus atomic replacement, so concurrent learning is merged instead of one process overwriting the other. These limits bound disk and startup cost without dropping new model results at the former 512-row candidate limit.

## Model Backends

Recent-history summaries overlap-deduplicate the rolling correction trail before counting keys. Only terminal candidate/raw commits confirm repeated corrections; intermediate snapshots and Escape remain context rather than learning evidence.

The main window has three user-facing pages. `学习` first shows whether automatic daily word-order learning is working, then reports how many complete inputs can update TiP and how many selection, English-token, and keyboard-correction habits TiP has activated. Diagnostic counters and the current raw snapshot are under the closed `高级详情` expander. `模型` presents only TiP, local-large-model, and cloud-large-model choices before opening the progressive settings form; `支持` shows the two installed payment codes in a wrapping layout. On the learning page, `更新 TiP` uses accumulated terminal samples to refresh the local artifact without analyzing the active request. It is separate from automatic LibIME history updates. `分析当前输入` is a separate model-page action that invokes the selected TiP, local, or cloud backend exactly once for the current bounded request. There is no combined train-and-analyze action.

TiPE separates input-method stability from model choice:

TiP does not rewrite the system LibIME/Rime dictionaries or LibIME's private user history. Those remain the stable candidate generator and ordinary local language-model learning layer. TiP learns bounded evidence for candidate pair ordering, keyboard-error corrections, exact English tokens, raw-English offering, and confirmed multi-step segment chains; repeated complete segment chains may additionally publish their combined phrase to TiPE's separate private user dictionary. Prediction applies the other components as a guarded reranking/correction layer over generated candidates. A cloud or local external model receives the same bounded snapshot and may return only candidate, correction, preference, or segment-chain protocol rows, which TiPE validates locally before applying or storing them.

- Built-in: lightweight local rules and correction learning, enabled by default and offline.
- Local external: choose the built-in `llama-cpp` mode for one-shot GGUF analysis, use Ollama for a persistent local service, or set `TIPE_MODEL_COMMAND` to a custom wrapper.
- Cloud external: select OpenAI or another OpenAI-compatible endpoint, enter the provider's model name and API Key, choose the permitted data categories, and run the no-network `检查填写内容` action before saving. A ChatGPT subscription is not an API Key. TiPE sends no network request until the user explicitly invokes analysis.
- Continuous mode never calls the configured external model command; it is reserved for bounded built-in learning and ranking. Use `F9` when you want the selected local or cloud model to run once for the current composition.
- Installed adapter: `~/.local/bin/tipe-model-adapter` can run in `heuristic`, `llama-cpp`, `ollama`, or `openai-compatible` mode. `llama-cpp` starts `llama-cli` only for the explicit analysis request, loads one selected GGUF file, filters its reply through the same candidate/correction safety boundary, and exits; there is no persistent model daemon. It defaults to CPU-only execution with 6 threads, an 8192-token context, and a 30-second engine timeout. The default `heuristic` mode is offline, validates the protocol, keeps raw-English ordering based on fixed tokens or learned preferences rather than focused-app differences, can immediately promote and persist an explicitly selected non-leading full-preedit candidate, can turn repeated supervised history pairs for the current preedit into `history_preference` learning signals, can emit current-preedit `preference` and `correction` rows already present in the request, can infer distinct safe typo corrections from the in-memory `correction_events` trail, can generalize repeated learned correction patterns such as missing letters at a stable absolute position or near the end of a preedit, and can use matching `segment_chain` rows to promote a combined candidate or, after a prefix commit, promote the remaining suffix candidate. The learning/supervision panel also surfaces repeated history pairs as `learnable-preference` rows so the window can show the exact suggested preference protocol before a model is called. This covers accepted alphanumeric-token digits such as `qwen2`, cursor insertion of missing letters, Delete removal of extra middle letters, and multi-step prefix selection chains without hard-coding one word.
- OpenAI-compatible authentication is read from the selected private key file or advanced environment-variable reference only for the explicit request. A stored key must be a regular, non-symlink, current-user-owned file with no group/other access and a bounded size. The adapter rejects malformed values and gives `curl` a mode-`0600` temporary header file instead of placing the bearer token in its command line; success, HTTP failure, interruption, and normal process exit all remove that file. The model settings panel and diagnostic output show only the key source and set/unset state, never the literal token.
- Cloud requests use the versioned `tipe.cloud-rerank.v1` prompt over Chat Completions. A fixed conservative system message is paired with structured JSON. The current preedit and bounded candidates are always present. Recent keys, edit trails, history evidence, and context are present only when `TIPE_MODEL_SEND_RECENT_INPUT=1`; cursor-surrounding text and the application name are present only when `TIPE_MODEL_SEND_SURROUNDING=1`. Both permissions default to `0` for configs created through `tipe-model-config`, and old cloud configs without either value are treated as private defaults by `tipe-model-current`. The response may contain only TiPE candidate, correction, preference, or segment-chain protocol rows; local validation filters it before ranking or learning. Each explicit analysis is one request, while continuous mode remains local-only.
- The adapter also reads TiPE's bounded `supervision-history.tsv`. In heuristic mode it can reuse repeated preedit/candidate selections and repeated correction trails. A history choice counts only when the event trail confirms the same `candidate-selected` or raw commit, so moving a highlight and abandoning it is not learned; passive default candidate-0 highlights are also ignored. HTTP dry-run requests expose repeated corrections in `recent_history_summary.top_corrections` and `behavior_summary.supervised_learning_signals`. External model prompts also include a top-level `learning_status` summary that classifies the clicked request as keyboard-context-only, ready-to-learn, awaiting suffix confirmation, selected-candidate-already-top, rank-only, or no-action. Newly learnable rows are exposed as `suggested_protocol`, while records already supplied as known preferences, corrections, or segment chains are exposed as evidence so wrappers can rank from them without echoing them back. This is the low-cost supervision path for habits such as repeatedly typing `ihao`, deleting it, and then typing `nihao`; the model sees a generic history-derived correction signal rather than a word-specific exception.
- TiPE's optional self-trained model is named **TiP**. It is trained only when `tipe-personal-model-train` or the supervision window's training action is used. It combines five bounded components: a hashed Chinese candidate ranker, a personal keyboard error channel, hashed exact English-token memory, an independent raw-English offer classifier, and a compact pinyin prior. Its input is not limited to completed pinyin: each terminal sample labels the bounded ordered key/action chain that led to the outcome, including edits, deletion, navigation, mode boundaries, candidate movement, and the final commit or cancel. Feature version 4 retains the ordered and optional context-fingerprint features from earlier artifacts, links a longer bounded correction/action trail to the chosen candidate, and distills aligned pinyin-syllable/Chinese-character evidence from both full and prefix selections. A repeated preference such as `chong -> 重` can therefore become evidence for `chongkai -> 重开` without a word-specific rule. Prefix-derived samples contribute only exact and phonetic-transfer features, not misleading full-composition rank or application features. Personal preedit/candidate pairs, context, ordered key trails, and remembered English tokens are stored only as sparse indices or fixed-size hashes, not recoverable source strings. The artifact separately retains aggregate one-edit letter rules and public pinyin spellings loaded from configured Rime dictionaries; these support keyboard correction but are not a replay of the user's text. Feature-version-1 through version-3 artifacts remain readable. New artifacts carry `"name":"TiP"`; legacy valid artifacts without a name remain loadable and are reported as TiP. Repeated exact choices still activate at count 2, or count 3 for raw text, without waiting for generic validation.
- Capability gates are independent. Keyboard correction requires repeated personal repair evidence plus the pinyin prior. Up to the newest 512 confirmed English-mode `raw-pass-through` commits provide quarter-weight auxiliary positives to TiP and train bounded exact-token memory. Repeating one exact non-pinyin English token at least three times can promote only that same text through a supervised raw-token record, even when it is too short for the generic identifier classifier; it does not unlock unseen-English generalization, and a repeated explicit Chinese choice for the same preedit suppresses the passive English memory. Raw-English generalization still requires candidates explicitly marked `raw-offer`, a balanced real accepted/rejected holdout, and zero false raw promotions. `capability-isolated-temporal-v4` excludes raw-candidate requests and derived prefix choices from Chinese generic validation, then requires at least five unseen unsupported non-leading Chinese choices, no original-first regression, and at least 60 percent accuracy. Derived prefix choices train exact prefix evidence and feature-version-4 phonetic transfer, but cannot claim whole-input validation because their labels compare only candidates that consume the same prefix. Generic Chinese safety can never unlock a raw offer; an unseen raw offer moves first only through repeated exact-token evidence or the validated raw profile. An independently safe keyboard-correction or raw-English component can still be published when the generic holdout is below its baseline; generic ranking remains locked and its weights stay inert. `inspect`, model config, doctor, and the Chinese supervision UI report exact English memory and all three generalized capabilities separately.
- Training first writes a candidate model beside the final file. Promotion is monotonic across safe keyboard correction, raw-English profiling, and generic Chinese ranking: a candidate that loses an already validated capability is reported as `preserved-safe-capability` and does not replace the current artifact unless `--force` is explicit. Before a normal promotion, the candidate also merges the valid existing model's bounded pair evidence, correction patterns, key habits, and pinyin prior by maximum count. Publication fails instead of silently dropping an active old row when a component limit is reached, so rotation of the bounded training history cannot erase established behavior. Model JSON is checked against the same 16 MiB limit before atomic replacement that the loader enforces afterward. A `safe-component-upgrade`, `safe-validation-upgrade`, or `safe-capability-upgrade` names a bounded migration and does not claim that Chinese generic ranking became ready. An active personal backend reads an atomically promoted model on its next explicit analysis request without restarting.
- The same explicit training action distills repeated non-pinyin English tokens, validated positional correction patterns, and global missing/extra/replacement/transposition habits into the lightweight C++ runtime. Normal typing then applies the bounded edit channel in-process without Python, a daemon, F9, or a cloud request. Position-specific evidence is preferred; global habits can repair an unseen position, and two different strong missing-key habits can be combined for longer input, as in `woxiangyo -> woxiangyong`. An exact known pinyin entry, any existing Chinese selection evidence for the current spelling, and a non-pinyin English token already committed as itself are protected from unrelated global habits. A proposed repair must also preserve the parsed pinyin-syllable count and lead to a decisive language-model or known-word result before it can replace a merely composable phrase. Short tokens stay on one-edit repairs to avoid compounding a real omission into an over-correction. Generated pinyin is filtered locally, with exact dictionary phrases ranked above merely parseable syllable sequences, then ranked by model evidence, capped, and stopped after the first viable correction so the measured hot-path overhead remains bounded. Raw correction pairs still provide exact repairs immediately, but once TiP runtime rules exist, noisy untrained pairs no longer create competing generic patterns. Distillation is idempotent, uses the same preference lock and atomic replacement as other learners, and preserves active old evidence. It runs after the model artifact is atomically published; a transient distillation failure is retried and reported as an incomplete optional synchronization rather than falsely reporting that the already-published TiP model failed. The next update retries it while retaining existing runtime rules. Use `--no-runtime-distill` when a model-only export must not update runtime preferences, or `--preferences PATH` for an isolated destination.
- TiP promotion is monotonic at every reported capability boundary, so harder new examples cannot silently disable previously validated behavior.
- A model with legacy validation metadata may receive one `safe-validation-upgrade` even when its ordinary recommendation is `collect-more-data`, provided the capability-isolated candidate has no first-choice regression and retains safe components. This removes target leakage without pretending that generic accuracy improved.
- TiP training takes a non-blocking user-only lock beside the model before exporting or training. A second concurrent training request exits clearly without creating a candidate model; normal prediction remains lock-free because publication is atomic.
- Installed config helper: `~/.local/bin/tipe-model-config` writes `~/.config/tipe/model-env`, and `~/.local/bin/tipe-model-current` reads it when used as `TIPE_MODEL_COMMAND`. It can select offline heuristic, TiP, one-shot `llama.cpp`, dump, a custom wrapper command, Ollama, or an OpenAI-compatible endpoint. A direct key can be stored with `--api-key-stdin`; only its private-file path is written to `model-env`, while `--api-key-env` remains available for externally managed secrets. The `llama-cpp` mode accepts a GGUF path plus optional `--llama-command`, `--llama-threads`, and `--llama-context` values; show/doctor output checks both the executable and model file before a real call. For personal mode, both `--show` commands validate the JSON rather than checking only file readability and report training samples, repeated pair evidence, whether generic ranking passed its safety gate, active correction patterns, holdout and baseline accuracy, validation gain, recommendation, and promotion margin; `tipe-doctor` reports the same metadata even while another backend remains active. `tipe-model-current --show` also reports the selected mode, backend, provider kind, configured command validity, endpoint URL, chat path, model name, API-key source and runtime set/unset status, current process command state and scope, activation hint, click trigger, and continuous-mode behavior from the exact config it will execute. `tipe-model-current --print-env` prints the sourceable model environment for inspection, including endpoint/model/temperature/token settings and only a key-file path or environment reference, never a literal secret. `tipe-model-config --show` reports matching `model-status` rows plus self-test and activation hints. The `process-command-active` row is scoped to the current shell only; use `tipe-doctor` without `--no-runtime` after a restart to inspect runtime model rows. Add `--test` to `--write` to run `tipe-model-self-test` against the selected config; with `--dry-run --test` it validates a temporary config without writing user files. For `llama.cpp`, Ollama, or OpenAI-compatible setup, use `--test-dry-run` to validate the generated model request JSON without loading a GGUF or making HTTP calls. Adapter request dry-run is rejected for heuristic, personal, custom, dump, and disabled modes; use the normal current-model self-test for those modes. Endpoint base URLs must be `http` or `https`, chat paths must start with `/`, and custom commands may include simple `KEY=value` prefixes and safe space-separated arguments; shell syntax belongs inside a wrapper script. `~/.local/bin/tipe-model-wrapper-new`, `tipe-model-wrapper-check`, `tipe-model-self-test`, and `tipe-model-replay` provide wrapper generation, validation, self-test, and replay without changing the active input session.
- Dump helper: `~/.local/bin/tipe-model-dump` atomically writes the exact model request TSV as a user-only `0600` file at `TIPE_MODEL_DUMP_PATH`, defaulting to `$XDG_CACHE_HOME/tipe/model-request.tsv`, `~/.cache/tipe/model-request.tsv`, or `/tmp/tipe/model-request.tsv` when no user cache path is available, and can emit `TIPE_MODEL_DUMP_RESPONSE` as a fake model reply.
- Explain/replay helpers: `~/.local/bin/tipe-model-explain` reads a dumped model request TSV and prints a compact TSV summary of preedit, candidates, visible/numbered rows, events, context, segment-selection chains, learned preference/correction totals, strongest learned rows, and the same behavior-summary style typo signals used by model prompts. Add `--panel` to emit stable `panel<TAB>...` rows for a learning/debug window, including model-input supervision rows, key-trail/correction-trail limits, current edit summaries, repeated correction-pattern summaries, realtime correction decisions with guard reasons, behavior counts, raw-English hints, recent segment-chain details, inferred possible corrections, and `panel<TAB>learning<TAB>status` rows that mirror the adapter/wrapper `learning_status` decision. Possible corrections are also emitted as `panel<TAB>learning<TAB>correction-signal` rows so the GTK summary can label them as learnable behavior rather than only raw diagnostics. `~/.local/bin/tipe-learning-panel` renders those rows as a compact read-only text panel, prints the raw panel protocol with `--raw-panel`, or opens the standalone GTK learning/debug window with `--window`; the GTK summary shows whether the current request is ready to learn, waiting for suffix confirmation, rank-only, or keyboard-context-only before any model is called. By default `tipe-learning-panel` does not call a model, but `--replay --check` can show the selected model output and wrapper-check result for the same dumped request, `--replay --explain-output` summarizes accepted candidate/correction/preference/segment-chain rows, and `--replay --dry-run-model` can preview the adapter's HTTP request JSON without making a network call. When `--raw-panel` or `--window` is combined with `--replay`, accepted model output is also exposed as `panel<TAB>model-output<TAB>...` rows so the window can supervise both TiPE's request and the model's response, including the learning file path and next-effect summary for accepted preferences and segment chains. In deferred replay window mode the window opens without calling a model; `Refresh` reloads the visible request, and `Analyze` runs the model once. `~/.local/bin/tipe-supervision-window` is the main clicked-learning launcher: it opens live supervision, falls back to the most recent request, and uses manual Analyze with learning enabled. `~/.local/bin/tipe-analyze-window` opens the same manual analysis window in deferred clicked mode by default, accepts `--learn-output` for clicked learning, and accepts `--run-on-open` only when opening the window should immediately run the configured model once. `~/.local/bin/tipe-model-replay --request PATH --check` sends that exact request to the configured model command and validates the returned rows before fcitx5 ever sees them; `--request -` reads a request from stdin and caches it for multi-pass check/learn flows. Add `--explain-output` to summarize accepted rows, `--learn-output` to persist accepted candidate/correction rows into TiPE preferences, or `--dry-run-model` to inspect and validate the generated adapter request instead of calling the endpoint.
- External model calls time out after 2 seconds by default. `tipe-model-config --write llama-cpp` selects 30 seconds because loading a GGUF per click is slower, and `tipe-restart-fcitx5` propagates that value into the engine that enforces it. Set `TIPE_MODEL_TIMEOUT_SECONDS` to a value from 1 to 30 when a wrapper needs a different limit.
- The installed adapter's HTTP call times out after 8 seconds by default. Set `TIPE_MODEL_HTTP_TIMEOUT_SECONDS` for adapter HTTP requests, and use `TIPE_MODEL_CHAT_PATH` when an OpenAI-compatible endpoint does not use `/chat/completions`.
- Set `TIPE_MODEL_DRY_RUN=1` with `TIPE_MODEL_BACKEND=llama-cpp`, `openai-compatible`, or `ollama` to print the request JSON without loading a GGUF or calling a model endpoint.
- `TIPE_MODEL_COMMAND` is executed directly without a shell. It may contain simple `KEY=value` environment prefixes and space-separated arguments made of safe path/word characters. Put shell logic, pipes, quoted arguments, tokens, and API calls inside a wrapper script, then point TiPE at that script.
- Model output may use LF or CRLF line endings. Unknown `TIPE_MODEL_BACKEND` values are rejected by the installed adapter instead of silently falling back. The installed adapter filters model output to existing candidates and plausible typo corrections before returning protocol rows to TiPE.
- Model requests include optional application and bounded surrounding text, short and long key trails, recent committed `context`, candidate metadata, segment chains, and validated evidence. The bounded recent commits and correction trail survive a normal post-commit fcitx reset for the same input context; destroying that input context clears them. Each request also carries opaque `context_features` and `surrounding_features`. Last/history persistence removes the raw `context`, `surrounding_before`, and `surrounding_after` rows but keeps those fingerprints. `tipe-training-export` ignores them by default; `--include-context` and `--include-surrounding` opt them into JSONL without restoring or retaining source text. Application names remain a separate `--include-application` choice.

The external command receives a TSV snapshot on stdin:

```text
protocol	1
preedit	nihao
application	Alacritty
surrounding_before	刚才
surrounding_after	后文
surrounding_features	before:v1:...	after:v1:...
candidates	你好	你号	拟好
state	preedit_cursor	5	candidate_cursor	0	expanded	0
runtime_state	continuous	0
selected_candidate	0	你好
visible_candidates	0:你好	1:你号	2:拟好
numbered_candidates	1:0:你好	2:1:你号	3:2:拟好
candidate_metadata	1	consumed_prefix	4	source	prefix	score	999999
events	letter:n	letter:i	cursor-move:Down	observed:Tab	...
event_counts	letter:2	cursor-move:1	observed:1
correction_events	letter:i	letter:h	letter:a	letter:o	backspace:	...	letter:n	letter:i	letter:h	letter:a	letter:o
correction_event_counts	letter:9	backspace:4
events	letter:q	letter:w	letter:e	letter:n	digit:2
context	刚才	你好
context_features	v1:...	v1:...
segment_chain	jixuzuo	jixu	继续	zuo	jixuzuo	继续做
preference	nihao	你号	3
correction	ihao	nihao	2
```

It may print any mix of these lines:

```text
candidate	你号
correction	ihao	nihao
preference	nihao	你号	4
segment_chain	nihao	ni	你	hao	nihao	你好	5
```

For compatibility, a plain candidate line such as `你号` is also accepted. `candidate` lines reorder the current candidate list and, when they promote a non-leading candidate whose preference is not already stored, are saved as lightweight preferences for the current preedit, so a clicked model suggestion can affect the next same input without calling the external model again. A candidate line that only repeats the current first candidate or an already stored preference is not persisted as learning. A model may also return the current preedit itself as a candidate when it is a likely raw English identifier, for example `candidate<TAB>started`; TiPE filters this narrowly and will not accept normal pinyin such as `nihao` through that path. `preference<TAB>started<TAB>started<TAB>4` is stored as the same raw-English preference kind as repeated intentional raw commits, while `preference<TAB>nihao<TAB>nihao<TAB>4` is rejected as normal pinyin raw text. `correction` lines teach TiPE a typo-to-preedit mapping and can refresh candidates immediately, including when the current typo has no candidates. `preference` rows explicitly ask TiPE to persist a current-preedit candidate preference, and `segment_chain` rows explicitly ask TiPE to persist a confirmed segmentation/correction chain. They are validated against the current request and existing preference file before being accepted: a `preference` row must match the current preedit and not already be stored, a `segment_chain` row must either match the current full preedit or match a current `pending_segment` plus an explicitly selected non-leading visible suffix candidate and not already be stored, generated candidates must already be in the current candidate list except for narrowly allowed raw-English current-preedit candidates and pending-segment combined phrases, and implied corrections must be plausible and not already stored.
In the request payload, event, context, segment-chain, candidate metadata, and surrounding text escapes `\`, tab, CR, and LF as `\\`, `\t`, `\r`, and `\n` so unusual key names or nearby text cannot break the TSV shape. `context_features` and `surrounding_features` use a stable versioned fingerprint and never contain the original text. They are pseudonymous training features rather than encrypted text; TiPE therefore keeps their use explicit in offline export. The remaining state, event, candidate metadata, preference, correction, and segment-chain rows retain their existing protocol meanings and safety checks.

Correction-pattern diagnostics use the same four edit kinds as realtime learning: `missing`, `extra`, `replace`, and adjacent `transpose`. Transpositions are printed as `gn->ng`, can aggregate across separate correction pairs, and remain bounded to preedits of at most 16 ASCII characters; one observation and English-like identifiers stay guarded.

Example optional model settings:

```bash
# Read or write the reusable installed model config. This does not restart fcitx5.
tipe-model-config --show
tipe-model-config --write heuristic
tipe-model-config --write dump --dump-path /tmp/tipe-model-request.tsv
tipe-model-wrapper-new --path ~/.local/bin/my-tipe-model-wrapper --configure
tipe-model-wrapper-check --command ~/.local/bin/my-tipe-model-wrapper
tipe-model-self-test
tipe-model-self-test --current
# Inspect collected terminal samples, then train without changing the active backend.
tipe-personal-model-train --dry-run
tipe-personal-model-train
# Optional private context features; source text is not emitted into JSONL or the model.
tipe-personal-model-train --include-context --include-surrounding
tipe-personal-model inspect
# The same opt-in can be stored for the supervision window's clicked training action.
tipe-model-config --write personal --training-context on --training-surrounding on
# Self-test covers ordinary candidate/correction output, stored segment-chain suffix replay,
# and pending-segment confirmation learning into a validated segment_chain row.
tipe-model-replay --request /tmp/tipe-model-request.tsv --check --explain
tipe-model-config --write custom --command ~/.local/bin/my-tipe-model-wrapper
tipe-model-config --write llama-cpp \
  --model ~/.local/share/tipe/models/qwen2.5-1.5b-instruct-q4_k_m.gguf
tipe-model-config --write ollama --base-url http://127.0.0.1:11434/v1 --model qwen2.5:0.5b
printf '%s\n' "$OPENAI_API_KEY" | tipe-model-config --write openai --model your-openai-model-name --api-key-stdin
printf '%s\n' "$PROVIDER_API_KEY" | tipe-model-config --write openai-compatible --base-url https://api.example.com/v1 --model your-provider-model --api-key-stdin
export TIPE_MODEL_COMMAND="$HOME/.local/bin/tipe-model-current"

# Offline protocol/heuristic adapter.
export TIPE_MODEL_COMMAND="$HOME/.local/bin/tipe-model-adapter"
export TIPE_MODEL_BACKEND=heuristic

# Capture exactly what TiPE would send to a model without calling any model.
export TIPE_MODEL_COMMAND="$HOME/.local/bin/tipe-model-dump"
export TIPE_MODEL_DUMP_PATH=/tmp/tipe-model-request.tsv
tipe-model-explain /tmp/tipe-model-request.tsv
tipe-model-replay --request /tmp/tipe-model-request.tsv --check

# One-shot local GGUF. llama-cli starts only for Analyze/F9 and exits afterward.
export TIPE_MODEL_COMMAND="$HOME/.local/bin/tipe-model-adapter"
export TIPE_MODEL_BACKEND=llama-cpp
export TIPE_MODEL_NAME="$HOME/.local/share/tipe/models/qwen2.5-1.5b-instruct-q4_k_m.gguf"
export TIPE_LLAMA_CPP_COMMAND=/usr/bin/llama-cli
export TIPE_LLAMA_CPP_THREADS=6
export TIPE_LLAMA_CPP_CONTEXT=8192
export TIPE_MODEL_TIMEOUT_SECONDS=30
TIPE_MODEL_DRY_RUN=1 tipe-model-adapter < /tmp/tipe-model-request.tsv

# Ollama or any OpenAI-compatible local endpoint.
export TIPE_MODEL_COMMAND="$HOME/.local/bin/tipe-model-adapter"
export TIPE_MODEL_BACKEND=ollama
export TIPE_MODEL_BASE_URL=http://127.0.0.1:11434/v1
export TIPE_MODEL_NAME=qwen2.5:0.5b
export TIPE_MODEL_CHAT_PATH=/chat/completions
export TIPE_MODEL_TEMPERATURE=0
export TIPE_MODEL_MAX_TOKENS=128
TIPE_MODEL_DRY_RUN=1 tipe-model-adapter < /tmp/tipe-model-request.tsv

# Official OpenAI endpoint. Put the token in the environment, not in TIPE_MODEL_COMMAND.
export TIPE_MODEL_COMMAND="$HOME/.local/bin/tipe-model-adapter"
export TIPE_MODEL_BACKEND=openai-compatible
export TIPE_MODEL_BASE_URL=https://api.openai.com/v1
export TIPE_MODEL_NAME=your-openai-model-name
export TIPE_MODEL_API_KEY="$OPENAI_API_KEY"

# Other OpenAI-compatible cloud endpoint.
export TIPE_MODEL_COMMAND="$HOME/.local/bin/tipe-model-adapter"
export TIPE_MODEL_BACKEND=openai-compatible
export TIPE_MODEL_BASE_URL=https://api.example.com/v1
export TIPE_MODEL_NAME=your-provider-model
export TIPE_MODEL_API_KEY=...
```

Use `./scripts/restart-fcitx5.sh` only when you are ready to restart the current fcitx5 session. The reusable config flow is now the default and sets `TIPE_MODEL_COMMAND=$HOME/.local/bin/tipe-model-current`; `--model-example` and `--model-adapter` are explicit development overrides, while `--no-model` intentionally disables the hook. Use `--dry-run` first to confirm the config path, then `tipe-doctor` after restart to check the runtime `model-command` and `model-config-active` rows. The config helper itself never restarts fcitx5, edits fcitx5 profile files, or calls model endpoints.

## Candidate UI

DBus toolkit clients such as browser text fields can advertise fcitx's `ClientSideInputPanel` capability. While `tipeui` is active, TiPE uses fcitx's custom input-panel callback for that capability so the toolkit cannot replace the TiPE candidate window with its own renderer; client-side preedit remains enabled. This route is selected by the frontend capability rather than an application name.

The current real-use UI is the `tipeui` fcitx5 addon. The engine publishes one complete `InputPanel` contract for every frontend: preedit and its edit cursor, the full candidate stream, selected index, expanded/collapsed state, supervision counters, continuous mode, and candidate selection callbacks. For `wayland_v2`, `tipeui` renders a dark TiPE-owned input-popup surface and follows the compositor text cursor through input-method-v2. For XIM, DBus, and other frontends it sends the same panel state to one GTK layer-shell helper. Both surfaces use the same Cairo/Pango renderer, fonts, clipping, visual cells, status badge, pointer hover, and left-click selection path; the GTK helper returns a snapshot-qualified candidate index to `tipeui`, which validates the current candidate before selecting it. The fallback accepts normal positive-height global rectangles and XIM spot locations whose protocol dimensions are zero. A non-XIM startup sentinel such as `38,38,0,0` stays invalid. If an XIM client supplies no spot at all (`0,0,0,0`), the helper first checks whether the active X11 process is Wine. For Wine it discovers that process's prefix, keeps one sleeping helper in the same prefix, and requests the standard MSAA `OBJID_CARET` rectangle plus an IMM-context boolean over a nonblocking pipe only when a candidate snapshot changes. It reads no application text and does not poll in the background. A valid nonzero MSAA rectangle is mapped from Xwayland physical pixels to GTK logical pixels. The IMM result is the placement contract: an IMM-aware control owns its rendered preedit and TiPE trusts the reported caret directly; a control without IMM support receives one deterministic Pango-measured advance and a transparent, input-inert pinyin overlay at the same MSAA insertion point. This keeps candidate and overlay geometry together without guessing from accessibility-coordinate jitter. If Wine or the application exposes no caret, TiPE captures the X11 pointer once as the last-resort preedit origin and advances from it by the Pango-measured text before TiPE's edit cursor. Near the right display edge, the GTK popup flips to the caret's left and keeps its right edge attached to the moving caret instead of becoming pinned to the screen edge. Normal Wayland input stays on the compositor-owned popup path because its text rectangle is surface-local, not a global layer-shell coordinate. `TIPE_WAYLAND_POPUP_EDGE_FALLBACK=1` keeps the old GTK edge path available only for controlled diagnosis. Normal input does not append engine/UI geometry logs. Start fcitx5 with `--debug` only for a bounded diagnostic run; then `tipeui.log`, `engine-trace.log`, and `candidate-window.log` are capped at 4 MiB.

Wine can receive client preedit callbacks only when fcitx5's XIM addon has `UseOnTheSpot=True` in `~/.config/fcitx5/conf/xim.conf`; the addon requires an fcitx5 restart after this change. TiPE's installer does not silently alter that global frontend setting. The transparent fallback above is still needed for Windows controls that negotiate callback preedit but expose no IMM context, including some Electron controls under Wine.

For temporary real-use testing without changing the saved fcitx5 profile, start fcitx5 with the TiPE UI override:

```bash
./scripts/restart-fcitx5.sh --debug --ui tipeui
```

The build also produces the required `tipe-candidate-window` fallback/diagnostic renderer for the TiPE-owned candidate panel and snapshot protocol:

```bash
./build/tipe-candidate-window --preedit nihao --candidates '你好,你号,拟好,倪浩,泥豪' --expanded
./build/tipe-candidate-window --preedit nihao --candidates '你好,你号,拟好,倪浩,泥豪' --cursor 900,680,2,18
./build/tipe-candidate-window --parse-snapshot $'nihao\t1\t7\t100\t200\t3\t18\t你好|你号|拟好|倪浩|泥豪|你好啊|你不好|你很好'
./build/tipe-candidate-window --parse-snapshot $'chang\t0\t0\t100\t200\t3\t18\t长句先选前半段后|短|后面的拼音不保留|候选|另外|更多'
./build/tipe-candidate-window --parse-snapshot $'nihao\t1\t7\t1240\t700\t2\t18\t你好|你号|拟好|倪浩|泥豪|你好啊|你不好|你很好' --layout-geometry 0,0,1280,720
./build/tipe-candidate-window --parse-snapshot $'nihao\t0\t0\t0\t0\t0\t0\t你好|你号' --layout-geometry 0,0,1280,720
./build/tipe-state-probe nihao --move Down --snapshot 100,200,3,18 | awk -F '\t' '$1 == "snapshot" {sub(/^snapshot\t/, ""); print; exit}' | ./build/tipe-candidate-window --parse-snapshot -
```

The GTK window is the shipping fallback for XIM, DBus, and other non-`wayland_v2` frontends, and remains a standalone snapshot diagnostic. While `tipeui` is active, the UI addon owns this helper and keeps it alive across compositions; clearing a preedit hides it and a later composition reuses it. A private event pipe carries only snapshot serials and candidate indexes back to `tipeui`, so mouse selection cannot bypass the engine's normal validation, prefix retention, or learning. `tipeui` remains the `wayland_v2` surface because its compositor popup can use surface-local caret coordinates directly.

For diagnosis only, `TIPE_NATIVE_FOLLOW_FALLBACK=1` applies when `tipeui` is not the active UI and asks fcitx5's native panel to show the current visual row for an unusable rectangle. During an explicit `--debug` run, engine-side decisions are logged to `$XDG_CACHE_HOME/tipe/engine-trace.log`, or `~/.cache/tipe/engine-trace.log` when `XDG_CACHE_HOME` is unset. A `popup	candidate-edge-fallback` row in `tipeui.log` means the explicitly enabled legacy Wayland edge diagnostic ran; normal non-Wayland frontend fallback is a separate path. `tipeui.log`, `candidate-window.log`, supervision snapshots, and engine traces all use the same TiPE cache directory so `tipe-doctor` can inspect them. The internal `TiPE`/`Eng` switch indicator uses the compositor popup on `wayland_v2` and the short-lived GTK status surface elsewhere; it is closed on deactivation.
Use `--parse-snapshot` to decode a snapshot argument, or `--parse-snapshot -` to read one snapshot line from stdin, without opening a GTK window. Its `candidate<TAB>index<TAB>selected<TAB>visible<TAB>shortcut<TAB>text` rows show which row owns the visible digit labels, and snapshots also print `cell<TAB>index<TAB>row<TAB>column<TAB>span` so multi-cell candidates and row-gap backfilling are testable in both collapsed and expanded mode. Each visible cell also has a `draw-cell<TAB>index<TAB>left<TAB>top<TAB>right<TAB>bottom<TAB>inside` row; `inside` is `1` when the rendered cell stays inside the measured panel padding. It also prints `layout<TAB>width<TAB>height<TAB>usable-cursor`; `usable-cursor` is `0` for frontend rectangles such as `0,0,0,0`, where TiPE will use fallback placement. The `edge-fallback<TAB>0|1` row predicts whether the engine should divert a usable near-edge cursor rectangle away from the Wayland popup path. Add `--layout-geometry X,Y,W,H` to also print the clamped global-screen `position<TAB>left<TAB>top<TAB>right<TAB>bottom` for that monitor rectangle, including monitors whose origin is not `0,0`. In a debug run, `tipeui.log` `popup rendered` rows include `expanded`, `rows`, `columnW`, `maxRight`, and `boundsOk`; `boundsOk=0` means the live Cairo candidate cells exceeded the panel padding.
