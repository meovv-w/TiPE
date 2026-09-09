# TiPE 输入法

TiPE 是本地优先的中文拼音输入法。核心状态机与 fcitx5 适配分离：按键热路径只更新 preedit、候选和内存状态；行为事件进入容量为 2048 的队列，由低优先级线程合并并写入个人词偏好。暂停时正在处理的批次会事务性回滚并回排，清空、禁用和回滚会取消并丢弃旧批次，不阻塞输入线程。个人数据通过临时文件再原子替换发布，学习线程失败不会影响输入。系统安装 LibIME 时使用其简体词典和语言模型，内置词表只作为无 LibIME 环境的回退。

## 当前可运行切片

已提供 `libtipe.so` fcitx5 输入法插件、`tipe-demo` 无桌面演示、`tipe-control` 简洁监督窗口和 `tipe-diagnose` 性能诊断。候选条默认横向，方向键或 Tab 后展开可移动列表；每页固定使用 `1` 到 `9` 和 `0`（第十项）编号，支持 PageUp/PageDown、Home/End 和 Shift+Tab 收起。拼音 preedit 另有独立 UTF-8 编辑光标，左右/Home/End 可移动，Backspace/Delete 在光标位置编辑，继续输入会插入当前位置。`candidate_grid` 模块提供动态行列、命中和翻页布局；`candidate_painter` 使用 Pango/Cairo 实际字体测量绘制紧凑/展开网格、HiDPI、选中高亮和鼠标命中，并有独立渲染回归。另提供独立公共 API `tipe-ui` addon：显式设置 `TIPE_UI_SNAPSHOT_DIR` 或 `TIPE_UI_EVENT_LOG` 时输出 HiDPI PNG/JSONL 诊断结果；设置 `TIPE_UI_X11=1` 且 X11/XWayland 可用时，会启用 override-redirect popup、光标矩形定位和边缘避让，并提供鼠标候选命中处理。缺少 DISPLAY 或连接失败时 `available=false`，标准 InputPanel 作为 fallback；当前 smoke 只验证连接、加载和安全卸载，未验证真实鼠标点击。Wayland 原生 popup surface 尚未实现。

## 构建与测试

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/tipe-demo
./build/tipe-diagnose
./scripts/install-preview.sh build
./scripts/install-user.sh build
```

在安装了 Fcitx5 测试前端模块的系统上，CTest 还会运行 `tipe-fcitx-tests`：它在独立 fcitx5 实例中验证真实按键、候选选择和 `commitString`。
当前工作区完整 CTest 为 20/20；其中 `tipe-ui-x11-smoke` 检查可选 popup addon 的连接/加载/卸载，`tipe-ui-page-offset-smoke` 覆盖公共候选列表第二页的偏移和高亮，`tipe-ui-headless-fallback` 覆盖无显示环境，`tipe-ui-wayland-capability-diagnostic` 验证公开 Wayland 连接能力和标准 InputPanel fallback 的诊断契约，`tipe-local-small-tests` 覆盖显式选择本地小模型后的候选查询，`tipe-resource-stress-tests` 覆盖有界后台资源压力，`tipe-cloud-transport-tests` 覆盖显式同意、脱敏 loopback HTTP、响应上限、超时和取消。设置 `TIPE_UI_SNAPSHOT_DIR` 和 `TIPE_UI_EVENT_LOG` 时，集成测试还会确认 PNG/JSONL 诊断输出确实产生。无 DISPLAY 时 `TIPE_UI_X11=1` 仍应安全回退。
`./scripts/run-protocol-regression.sh build` 额外运行 GTK、Qt、终端能力画像，记录当前运行时版本并生成 `build/protocol-regression.log`；该脚本验证协议行为，不代替真实窗口的视觉回归。
可选的真实 toolkit harness 可运行 `./scripts/run-real-app-harness.sh build`，收集 GTK4/Qt6 输入上下文的 text rectangle、preedit/commit、surrounding-text 和光标查询。它不会切换当前 profile 或重启 fcitx；只有当前会话已手动选择 TiPE 时，输出才包含 TiPE 的真实输入事件。本机已确认 GTK4 4.22.4、Qt6 6.11.1 和 foot 1.27.0 可用，但当前会话使用 `keyboard-us`，因此本次 harness 输出只证明窗口和 toolkit 信号路径，不构成 TiPE 真实应用验收。
需要验证多段中文/英文混合和真实光标删除时，可运行 `./scripts/run-real-app-edit-regression.sh build build/isolated-edit-regression`；它在隔离 GTK4/Qt6 窗口中检查 `你好界github你好` 最终文本。
工作区还提供只观察真实 toolkit 的最小 harness：`tipe-gtk-harness` 和 `tipe-qt-harness` 通过正常 GTK/Qt 输入法上下文记录 preedit、commit、surrounding-text、光标查询和文本矩形。运行 `./scripts/run-real-app-harness.sh build build/real-app-harness` 会记录当前 GTK/Qt/foot 版本和输入法状态；脚本不会切换 profile 或重启 fcitx。只有在用户已手动选择 TiPE 时，JSONL 中的 preedit/commit 才可作为 TiPE 应用侧证据。详见 [docs/REAL-APP-HARNESS.zh-CN.md](docs/REAL-APP-HARNESS.zh-CN.md)。
如果不能触碰当前桌面 profile，可运行 `./scripts/run-isolated-real-app-harness.sh build build/isolated-real-app-harness`。它在临时 XDG 配置、独立 D-Bus 会话和嵌套 Xwayland（有 Xvfb 时优先使用 Xvfb）中启动私有 fcitx5，写入临时 `DefaultIM=tipe` profile，并要求 GTK4、Qt6 都产生 `你好` 的真实 toolkit commit；当前用户的 profile、fcitx5 进程和桌面 keybinds 不会被修改。

该隔离脚本同时写入 `chromium-smoke.log` 和 `foot-smoke.log`。Chromium 使用临时浏览器 profile，并通过页面标题记录输入框是否观察到 `你好`；不同发行版的 Chromium 可能不启用 X11 IME，因此 `commit-not-observed` 是明确的未观察结果。Foot 是 Wayland 原生终端，嵌套 Xwayland 运行会记录 `no-Wayland-display-for-foot`，不会把这类跳过当成 Wayland 验收。设置 `TIPE_REAL_APP_STRICT_APPS=1` 可将已找到窗口但未观察到提交变为失败。
在有 X11/XWayland 显示的环境中，`TIPE_UI_X11=1 DISPLAY=:0 ctest --test-dir build -R '^tipe-ui-x11-smoke$' --output-on-failure` 已通过 smoke 回归，确认可选 `tipe-ui` addon 能连接、加载并安全卸载 override-redirect popup；设置事件日志时还会记录 popup 是否可见及其位置/尺寸。这不等于真实桌面中的候选视觉或鼠标回归。需要 PNG/JSONL 诊断帧时，设置 `TIPE_UI_SNAPSHOT_DIR` 或 `TIPE_UI_EVENT_LOG` 后运行同一测试。

安装预演写入 `/tmp/tipe-staging`，不会修改 niri 或系统级 fcitx5。当前用户安装使用 `scripts/install-user.sh`；输入法目录搜索受桌面会话环境影响时，用 `scripts/run-user-fcitx.sh` 启动用户级 fcitx5。`scripts/enable-user.sh` 会备份并把 TiPE 加入当前用户 profile，`scripts/disable-user.sh` 可撤销。卸载预演：`./scripts/uninstall-preview.sh`。

## 学习与隐私

默认只保存本地事件计数和用户明确提交的短文本；队列有界，文件写入使用 `.tmp` 后原子替换。状态位于 `$XDG_STATE_HOME/tipe`（未设置时为 `~/.local/state/tipe`），控制中心可暂停/恢复、关闭/开启学习、清空、导出和回滚。云端模型默认不可用且不会发送键盘流；底层 `CloudTransport` 仅在调用方同时启用传输并明确同意后发送脱敏语义事件，当前只接受受限 loopback/HTTP 测试端点，HTTPS/TLS 和真正云端后端仍未接入用户界面。基础词库与个人偏好保持独立，重复三次以上的精确个人词才会对同一拼音/场景加入候选；不会改写基础词库。当前还没有完整的置信度模型或泛化纠错系统，但已提供受限漏键规则：同一场景需至少 6 次支持和 3 种修正拼音，只在 LibIME 仅返回原文时建议，并有容量上限。

资源边界已有窄范围证据：`tipe-learning-tests` 向一个临时状态目录连续推送 12000 个不同拼音，确认事件队列最多 2048 项、模型最多 4096 条、当前 regular-file 总量低于 8 MiB，并核对目录扫描值与 `disk_bytes()` 相等。`tipe-resource-stress-tests` 会持续分批推送事件、反复发布模型并在运行期间采样队列、模型条目、目录大小、`/proc` VmRSS 和 CPU/wall；默认 CTest 运行 120 轮，脚本可用 `TIPE_RESOURCE_STRESS_ROUNDS`、`TIPE_RESOURCE_STRESS_EVENTS`、`TIPE_RESOURCE_STRESS_PAUSE_MS` 延长 soak（例如 `TIPE_RESOURCE_STRESS_ROUNDS=600 scripts/run-resource-stress.sh build`）。当前实现的硬回归边界为队列 2048、模型 4096、regular-file 总量 8 MiB、采样 RSS 256 MiB；CPU 只记录样本并用很宽的 runaway guard，不宣称长期 CPU 上限。`tipe-diagnose` 的 RSS/CPU 仍是单进程样本；断电恢复和真实桌面持续资源监控仍需单独验收。

模型选择状态保存在同一目录的 `models.current`，通过临时文件原子替换。TiP 个人增量层默认启用；选择“本地小模型”后，TiPE 会从同目录的 `local-small.tsv` 读取有界候选表（文件不存在时使用内置的极小安全表），在内存中查询并置顶匹配词。该后端不启动外部进程、不联网、不写入个人学习模型；文件格式为 `拼音<TAB>词<TAB>分数[<TAB>上下文]`，每次最多读取 4096 行。云端 API Key 仅记录路径且不触发网络请求；云端 backend 仍不可选，但脱敏 loopback transport 的取消、超时和响应上限已有自动测试。

## 已知限制

当前版本已接入系统 LibIME 简体词典和 `zh_CN.lm`，内置词表作为回退；已有进程级突然退出后的原子模型恢复回归，且学习测试用独立 writer/control 子进程验证了共享控制文件的暂停、批次回滚和恢复。上述证据尚未覆盖长期断电、SIGKILL 落在每个 fsync/rename 窗口时的完整恢复协议或持续资源监控。Wayland 原生自绘网格和完整真实应用回归仍未完成。X11 popup 已通过独立 fcitx5 smoke 验证窗口连接、加载和安全卸载，但尚未完成真实 GTK/Qt/浏览器/Discord/Wine 的视觉与鼠标回归。真实切换前请先完成安装预演，并保留 profile 备份。
