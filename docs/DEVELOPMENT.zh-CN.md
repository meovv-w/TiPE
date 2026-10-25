# 开发与验证参考

以下命令中的路径均相对于仓库根目录。产品概览见 [首页](../README.md)。

# TiPE 输入法

TiPE 是本地优先的中文拼音输入法。核心状态机与 fcitx5 适配分离：按键热路径只更新 preedit、候选和内存状态；行为事件进入容量为 2048 的队列，由低优先级线程合并并写入个人词偏好。暂停时正在处理的批次会事务性回滚并回排，清空、禁用和回滚会取消并丢弃旧批次，不阻塞输入线程。个人数据通过临时文件再原子替换发布，学习线程失败不会影响输入。系统安装 LibIME 时使用其简体词典和语言模型；启动时会检查词库/语言模型是否存在且可读，缺失或损坏时记录诊断并回退到有界内置词表，不让输入法进程退出。

## 当前实现

按用户2026-10-23确认的范围，交付已完成。实体 niri 多屏与 Discord 登录聊天不再作为验收条件，仍明确记录为未验证。详情见 [交付说明](DELIVERY.zh-CN.md) 和 [验收状态](ACCEPTANCE.md)。

已提供 `libtipe.so` fcitx5 输入法插件、`tipe-demo` 无桌面演示、`tipe-control` 简洁监督窗口和 `tipe-diagnose` 性能诊断。TiPE 自绘候选条默认是紧凑单行，方向键或 Tab 后展开可移动网格；GTK、Qt 和带补丁的 classicui 使用黑色候选窗，展开时保持紧凑条的列数、各列宽度和窗口宽度，只向下增加候选行；`scripts/run-user-fcitx.sh` 默认启用 `TIPE_CLASSIC_GRID=1`，未安装补丁时退回 Fcitx 自带布局。`scripts/install-user.sh` 会安装随项目附带的 MacOS-Dark 深灰圆角主题；已有明确 Theme 配置时会保留用户选择。紧凑态每页 10 个候选，展开态每页最多 40 个；数字 `1` 到 `9` 和 `0` 选择当前高亮所在十项组内的候选，只有这一组显示编号，支持 PageUp/PageDown、Home/End 和 Shift+Tab 收起。拼音 preedit 另有独立 UTF-8 编辑光标，左右/Home/End 可移动，Backspace/Delete 在光标位置编辑，继续输入会插入当前位置；核心也统一接受常见 GTK/Qt/X11 按键别名（如 `Backspace`、`Escape`、`Return`、`KP_Enter`、`Page_Up` 和 `ISO_Left_Tab`）。`candidate_grid` 模块提供动态行列、命中和翻页布局；`candidate_painter` 使用 Pango/Cairo 实际字体测量绘制紧凑/展开网格、HiDPI、选中高亮和鼠标命中，并有独立渲染回归。另提供独立公共 API `tipe-ui` addon：显式设置 `TIPE_UI_SNAPSHOT_DIR` 或 `TIPE_UI_EVENT_LOG` 时输出 HiDPI PNG/JSONL 诊断结果；设置 `TIPE_UI_X11=1` 且 X11/XWayland 可用时，会启用 override-redirect popup、光标矩形定位和边缘避让，并提供鼠标候选命中处理。缺少 DISPLAY 或连接失败时 `available=false`，标准 InputPanel 作为 fallback；当前 smoke 只验证连接、加载和安全卸载，未验证真实鼠标点击。GTK/Qt 的客户端 popup 及原生 text-input 的 classicui popup 已接入同一绘制器；独立诊断用 tipe-ui 的公开 API 不具备原生 popup 工厂。

## 构建与测试

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/tipe-demo
./build/tipe-diagnose
./build/tipe-diagnose --trained-local --representative
./scripts/install-preview.sh build
./scripts/install-user.sh build
```

控制中心的实际 Qt 窗口截图可独立复现，不读取个人学习记录：

```bash
cmake --build build --target tipe-control-window-tests
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 \
  TIPE_CONTROL_CAPTURE_DIR=build/control-capture ./build/tipe-control-window-tests
python3 scripts/render-readme-assets.py --control-captures build/control-capture
```

设置 `TIPE_CONTROL_LARGE_FONT=1` 可检查大字体；窗口测试也会检查 480×580
窄窗口下三个页面的滚动与设置按钮可达性。截图来自独立测试数据，不能代替真实
桌面合成器的显示检查。2026-10-26 的结果见
[界面验证记录](evidence/2026-10-26/control-redesign-results.json)。

候选方向键回归可在隔离的 niri 桌面运行：
`python3 scripts/check-niri-isolated.py --matrix arrows --frontends gtk qt browser --scales 1`。
它使用安装后的模块、独立输入上下文和临时学习目录，固定基础候选顺序，检查右键
进入候选、左键返回、收起后空格选词、Enter 原文及组合内编辑，并通过截图核对
高亮横向移动。2026-10-26 的 [27 项结果](evidence/2026-10-26/compact-arrows-results.json)
与 [6 项核心及适配测试](evidence/2026-10-26/compact-arrows-tests.txt) 已保存。

需要按键级候选和分段边界诊断时，可运行
`./build/tipe-demo --json-lines --state-dir /tmp/tipe-demo-private`，标准输入仍为
空格分隔的按键名。每行JSON包含提交、剩余拼音、编辑光标和候选的真实消耗长度。
为避免演示输入进入个人学习，使用单独的状态目录。

长组合不会因超过64字节而失去中文候选：基础解码处理前70字节，在64字节内
的真实词边界分段提交，其余拼音完整保留。编辑只把变化区间交给增量解码器。
基础词库和语言模型在同一线程的输入上下文间共享，组合与个人选择各自独立；
Fcitx加载插件时预热词库，避免首次按键重复加载。`--representative`额外测量
普通句子、长词、68/242字节组合、重复拼写、英文、网址，以及每轮不同的20条长拼写，
记录输入、展开、导航与中间编辑的p50/p95/p99和最大值；不把短`nihao`样本当成全部输入的保证。
基础模型省去恒为空的个人历史查询，保持原始评分；展开前缀用集合去重并在同一组合内
有界复用，个人模型改变排序时重新生成，重置或隐私边界变化时清理。

桌面编辑、分段选择和自动学习链的入口是`scripts/check-editing-desktop.py`，
分别使用`--matrix editing`、`--matrix segmented`和`--matrix chain-learning`。
GTK/Qt四角与长候选可运行`TIPE_POPUP_FULL_MATRIX=1 python3 scripts/check-popup-desktop.py gtk`
（或`qt`）。先结束CTest，再顺序运行桌面检查；详情见`docs/REAL-APP-HARNESS.zh-CN.md`。
原生Chromium/foot四角、长句和鼠标选择检查可运行
`python3 scripts/check-native-popup-desktop.py browser foot`，当前单屏2×的16项已通过；
该检查不覆盖多屏或混合缩放。
`python3 scripts/check-multioutput-desktop.py` 使用隔离Mutter双屏与虚拟输入设备，
检查实际安装GTK/Qt模块的活动组合双向跨屏、1.25×/1.5×/2×缩放和鼠标选词；
24项已通过。该证据不代表实体niri多屏或XIM多屏验证，详见验收表。
`python3 scripts/check-native-multioutput.py` 另以私有Sway双屏检查foot的原生
input-popup，三种混合缩放下12项通过；既有组合在双向跨屏时保留，逻辑宽度、
协议光标锚点和键盘/鼠标提交均核对。依赖与证据范围见真实应用harness说明。
加`--frontend wine`可检查隔离Xwayland的标准Win32 EDIT，同样12项已通过。
修复通过父窗口结构事件重绘候选子窗，避免跨屏后内容存在却未显示到桌面。

在安装了 Fcitx5 测试前端模块的系统上，CTest 还会运行 `tipe-fcitx-tests`：它在独立 fcitx5 实例中验证真实按键、候选选择和 `commitString`。
当前工作区 CTest 共 36 项，最新完整运行 36/36 通过（82.36 秒），见 `docs/evidence/2026-10-23/ctest-prefix-linear-final.log`。其中基础解码优化与原始LibIME对比70组拼写、35,664次逐词浮点评分；`tipe-decoder-resource-tests`覆盖12上下文词库共享、独立组合、缺失/替换/损坏恢复。新增 `tipe-user-installation` 包含 10 个隔离测试，覆盖真实安装/卸载入口、用户修改保护、旧清单迁移、原子替换、进程中断恢复、事务失败回滚和多输入法组保留。其中 `tipe-ui-x11-smoke` 检查可选 popup addon 的连接/加载/卸载，`tipe-ui-page-offset-smoke` 覆盖公共候选列表第二页的偏移和高亮，`tipe-ui-headless-fallback` 覆盖无显示环境，`tipe-ui-wayland-capability-diagnostic` 验证公开 Wayland 连接能力和标准 InputPanel fallback 的诊断契约，`tipe-wayland-capability-probe` 只读记录 compositor registry 能力，`tipe-local-small-tests` 覆盖显式选择本地小模型后的候选查询，`tipe-resource-stress-tests` 覆盖有界后台资源压力，`tipe-cloud-transport-tests` 覆盖显式同意、脱敏 loopback HTTP/HTTPS、响应上限、超时和取消，`tipe-cloud-worker-tests` 覆盖后台队列与 Learner 接线，`tipe-cloud-candidate-tests` 覆盖严格响应 schema、UTF-8/上下文校验、TTL 和容量回收，`tipe-libime-fallback-tests` 覆盖词库缺失或损坏时的安全回退，`tipe-quality-isolation-tests` 覆盖 60 条 LibIME 质量语料、异上下文噪声隔离、精确偏好和重启恢复，`tipe-english-corpus-tests` 覆盖英文、代码标识符、网址、路径与拼音负向语料，`tipe-fcitx-lifecycle-tests` 覆盖切换输入法、失焦只提交一次、无 inline preedit 时提交原文、焦点上下文销毁重建和状态隔离；`tipe-daemon-mapping` 与 `tipe-activation-readiness` 验证旧映射拒绝、启动进程接管与 PID 0 保护，`tipe-qt-surrounding-fallback` 覆盖真实 Qt 控件无法提供 surrounding-text 时的客户端编辑回退。设置 `TIPE_UI_SNAPSHOT_DIR` 和 `TIPE_UI_EVENT_LOG` 时，集成测试还会确认 PNG/JSONL 诊断输出确实产生。无 DISPLAY 时 `TIPE_UI_X11=1` 仍应安全回退。
其中英文离线评估逐字运行 819 条英文/代码/网址/路径正向样本和 306 条普通拼音负向样本；当前 LibIME 运行时两类误判均为 0。
`./scripts/run-protocol-regression.sh build` 额外运行 GTK、Qt、终端能力画像，记录当前运行时版本并生成 `build/protocol-regression.log`；该脚本验证协议行为，不代替真实窗口的视觉回归。
在原生 Wayland 桌面可运行 `scripts/probe-wayland-capabilities.sh build/wayland-capability-probe`，或运行 CTest 中的 `tipe-wayland-capability-probe`。脚本只读取 registry（不会绑定输入法对象、创建 surface 或修改桌面配置），输出 `capabilities.json` 和原始 `wayland-info.txt`。`input_method_manager_v2:true` 证明 compositor 公布了输入法管理器；`native_popup_created:false` 是有意的，因为候选 popup 仍由 Fcitx 私有 `waylandim` 工厂创建，registry 探测本身不能冒充原生 popup 视觉证据。没有 Wayland socket 时结果为 `status:skipped`，不会阻止 TiPE 启动。
GTK/Qt 的 `IM_MODULE=fcitx` 使用客户端自绘 Wayland popup；`tipe-client-side-adapter-tests` 验证了把 Fcitx `UpdateClientSideUI` 可见页转换为 TiPE 网格时的页内 cursor、Vertical 展开和缩放边界。该适配层不创建第二个 input-method，集成说明见 [docs/FCITX-WAYLAND-ADAPTER.zh-CN.md](FCITX-WAYLAND-ADAPTER.zh-CN.md)。
需要整理离线语义样本时可运行 `build/tipe-trainer --state-dir DIR --input FILE`。输入文件使用 `拼音<TAB>词<TAB>上下文`，每行代表一次明确选择；工具复用同一个低优先级 Learner、队列上限和原子发布，不会启动第二个 writer。收到 SIGINT/SIGTERM 时先暂停，传入 `--rollback-on-cancel` 才会回滚上一版模型。
可选的真实 toolkit harness 可运行 `./scripts/run-real-app-harness.sh build`，收集 GTK4/Qt6 输入上下文的 text rectangle、preedit/commit、surrounding-text 和光标查询。它不会切换当前 profile 或重启 fcitx；只有当前会话已手动选择 TiPE 时，输出才包含 TiPE 的真实输入事件。本机已确认 GTK4 4.22.4、Qt6 6.11.1 和 foot 1.27.0 可用，原生桌面的 GTK4/Qt6 候选窗证据已在 docs/evidence/2026-10-21 保存。
需要验证多段中文/英文混合和真实光标删除时，可运行 `./scripts/run-real-app-edit-regression.sh build build/isolated-edit-regression`；它在隔离 GTK4/Qt6 窗口中检查 `你好界github你好` 最终文本。
工作区还提供只观察真实 toolkit 的最小 harness：`tipe-gtk-harness` 和 `tipe-qt-harness` 通过正常 GTK/Qt 输入法上下文记录 preedit、commit、surrounding-text、光标查询和文本矩形。运行 `./scripts/run-real-app-harness.sh build build/real-app-harness` 会记录当前 GTK/Qt/foot 版本和输入法状态；脚本不会切换 profile 或重启 fcitx。只有在用户已手动选择 TiPE 时，JSONL 中的 preedit/commit 才可作为 TiPE 应用侧证据。详见 [docs/REAL-APP-HARNESS.zh-CN.md](REAL-APP-HARNESS.zh-CN.md)。
如果不能触碰当前桌面 profile，可运行 `./scripts/run-isolated-real-app-harness.sh build build/isolated-real-app-harness`。它在临时 XDG 配置、独立 D-Bus 会话和嵌套 Xwayland（有 Xvfb 时优先使用 Xvfb）中启动私有 fcitx5，写入临时 `DefaultIM=tipe` profile，并要求 GTK4、Qt6 都产生 `你好` 的真实 toolkit commit；当前用户的 profile、fcitx5 进程和桌面 keybinds 不会被修改。
隔离运行结束会删除临时根目录及 Chromium/Wine 缓存，不会在 `/tmp` 累积数据。

该隔离脚本同时写入 `chromium-smoke.log` 和 `foot-smoke.log`。Chromium 使用临时浏览器 profile，并通过页面标题记录输入框是否观察到 `你好`；不同发行版的 Chromium 可能不启用 X11 IME，因此 `commit-not-observed` 是明确的未观察结果。Foot 是 Wayland 原生终端，嵌套 Xwayland 运行会记录 `no-Wayland-display-for-foot`，不会把这类跳过当成 Wayland 验收。设置 `TIPE_REAL_APP_STRICT_APPS=1` 可将已找到窗口但未观察到提交变为失败。
在有 X11/XWayland 显示的环境中，`TIPE_UI_X11=1 DISPLAY=:0 ctest --test-dir build -R '^tipe-ui-x11-smoke$' --output-on-failure` 已通过 smoke 回归，确认可选 `tipe-ui` addon 能连接、加载并安全卸载 override-redirect popup；设置事件日志时还会记录 popup 是否可见及其位置/尺寸。这不等于真实桌面中的候选视觉或鼠标回归。需要 PNG/JSONL 诊断帧时，设置 `TIPE_UI_SNAPSHOT_DIR` 或 `TIPE_UI_EVENT_LOG` 后运行同一测试。

安装预演写入 `/tmp/tipe-staging`，不会修改 niri 或系统级 fcitx5。当前用户安装使用 `scripts/install-user.sh`；要把 TiPE 切入当前桌面，使用 `scripts/activate-user.sh`，它会停止旧 daemon、等待退出、写入带备份的 profile、只启动一个带用户插件路径的 fcitx5，并确认当前输入法确实为 `tipe`，避免 profile/daemon 竞态。`scripts/enable-user.sh` 与 `scripts/disable-user.sh` 是停 daemon 后使用的底层 profile 编辑命令，检测到 fcitx5 运行时会拒绝执行。卸载预演：`./scripts/uninstall-preview.sh`。实际卸载先用 `./scripts/uninstall-user.sh --dry-run` 查看，再运行 `./scripts/uninstall-user.sh`；它会在线移除各组中的 TiPE、保留其他输入法，按校验清单恢复或移除用户文件，保留学习数据和用户后续修改。安装先在临时目录 staging，再通过同目录原子替换发布；持久事务日志可恢复中断的安装。安装器也会保存现有 `fcitx5.service` 的可恢复 drop-in，让下次登录仍加载用户 TiPE 模块；安装/卸载只重载服务定义。无该服务的桌面使用 `scripts/run-user-fcitx.sh` 启动。`$XDG_CONFIG_HOME`、`$XDG_DATA_HOME` 和 `$XDG_STATE_HOME` 均受支持。

## 学习与隐私

默认只保存本地事件计数和用户明确提交的短文本；队列有界，文件写入使用 `.tmp` 后原子替换。状态位于 `$XDG_STATE_HOME/tipe`（未设置时为 `~/.local/state/tipe`），控制中心可暂停/恢复、关闭/开启学习、清空、导出和回滚。云端增强默认关闭；只有同时设置 `TIPE_CLOUD_ENABLED=1`、`TIPE_CLOUD_CONSENT=1` 和 `TIPE_CLOUD_ENDPOINT=https://...` 才会启用 `CloudWorker`。它只接收提交/选词语义事件，使用独立有界队列、5 秒成功请求间隔、取消和指数退避，网络请求永远不在按键线程执行。API key 可通过 `TIPE_CLOUD_API_KEY` 指向权限为 0600 的文件，CA 可通过 `TIPE_CLOUD_CA` 指定；后台状态写入 `cloud.state`，控制中心会显示发送、失败和重试计数。基础词库与个人偏好保持独立，重复三次以上的精确个人词才会对同一拼音/场景加入候选；不会改写基础词库。纠错覆盖漏字、多字、替换和相邻字母颠倒：同一场景需至少 6 次支持、3 种修正拼音及不低于 0.8 的竞争证据置信度，只在 LibIME 仅返回原文时介入。推断结果不反过来训练自身。原文提交习惯仅按同一拼写和应用学习，原文优先时保留中文候选，明确选择中文可撤销该偏好。

资源边界已有窄范围证据：`tipe-learning-tests` 向一个临时状态目录连续推送 12000 个不同拼音，确认事件队列最多 2048 项、模型最多 4096 条、当前 regular-file 总量低于 8 MiB，并核对目录扫描值与 `disk_bytes()` 相等。`tipe-resource-stress-tests` 会持续分批推送事件、反复发布模型并在运行期间采样队列、模型条目、目录大小、`/proc` VmRSS 和 CPU/wall；默认 CTest 运行 120 轮，脚本可用 `TIPE_RESOURCE_STRESS_ROUNDS`、`TIPE_RESOURCE_STRESS_EVENTS`、`TIPE_RESOURCE_STRESS_PAUSE_MS` 延长 soak（例如 `TIPE_RESOURCE_STRESS_ROUNDS=600 scripts/run-resource-stress.sh build`）。当前实现的硬回归边界为队列 2048、模型 4096、regular-file 总量 8 MiB、采样 RSS 256 MiB；学习线程按每批实际 CPU 时间等长让出，回归测试要求 worker duty 不超过 75%。`tipe-diagnose` 的 RSS/CPU 仍是单进程样本；故障注入不等同物理断电，资源测量只代表所列采样负载。

模型选择状态保存在同一目录的 `models.current`，通过临时文件原子替换。TiP 个人增量层默认启用；选择“本地小模型”后，TiPE 使用后台从重复确认词句中生成的字符二元语言模型，按应用范围评分较低位的完整候选，保护基础首选和前缀边界；上限 16 个应用、4096 个字符转移。模型随个人词版本原子发布、重启重建、清空和回滚。同目录的 `local-small.tsv` 是可选的显式领域词表，并非语言模型；该后端不启动外部进程、不联网；文件格式为 `拼音<TAB>词<TAB>分数[<TAB>上下文]`，每次最多读取 4096 行。云端候选后端只有在显式环境门控和同意都存在时才可选；经过严格 schema 校验的结果进入独立短期缓存并追加到本地候选之后，不污染 LibIME 或 TiP。CloudWorker 和脱敏 HTTP(S) transport 已作为独立后台传输层实现并有 TLS、取消、超时、权限和响应上限测试。

原生 Wayland 的可选 `fcitx5-wayland-activation.patch` 修复首次输入前的状态确认，并在字段用途缺失时暂时走原文且停止学习；收到用途后恢复。它与 classicui 一起从固定 Fcitx 5.1.21 源码构建，由安装器纳入备份与卸载。Chromium 150 密码框已验证首字符完整、观察计数不变、无测试内容进入模型，返回普通文本框能继续中文并恢复学习；空闲时无协议确认循环。GTK/Qt/Wine 密码夹具也通过相同隐私检查。XIM 不能标记密码框，因此 TiPE 默认对整个 XIM 路径关闭观察、个人学习和云端工作，保留中文输入。

## 已知限制

当前版本已接入系统 LibIME 简体词典和 `zh_CN.lm`，内置词表作为回退；学习测试用独立 writer/control 子进程验证了共享控制文件的暂停、批次回滚和恢复。额外的 Debug 恢复构建会让 SIGKILL 落在每个 fsync/rename 窗口，再验证重启后可读取完整代次并继续写入。该测试不等同于真实断电。

GTK/Qt、Chromium 和 foot 原生 Wayland 输入已实测；GTK4/Qt6 的右下、左上测试窗口已验证展开宽度不变、屏幕底部向上避让和真实鼠标/键盘选词一致。Wine 标准 EDIT 控件在同一输入法进程下连续两次关闭重开，均通过候选可见、Enter 原文、Space 中文、中英切换及鼠标选词；当前单屏2×缩放已核对。Wine 默认 XIM 桥接可能只提供初始光标坐标；[按应用配置工具](WINE.zh-CN.md) 可启用标准 `overthespot` 模式，已验证光标横移、换行及19项编辑，并可恢复原值。隔离Xwayland双屏三种混合缩放已通过12项，实体niri多屏仍未验证。Discord 只验证了隔离启动器 Electron 渲染器中的本地输入框，完整登录聊天界面未验收。云端协议使用本地 HTTP(S) 服务验证，尚未配置外部生产服务。证据、用户确认的范围及限制见 [验收状态](ACCEPTANCE.md)。真实切换前请先完成安装预演，并保留 profile 备份。

实现与当前验证说明见 [交付说明](DELIVERY.zh-CN.md)，安装和数据操作见 [白话用户说明](USER_GUIDE.zh-CN.md)。

已安装niri 26.04也有可重复的隔离入口`python3 scripts/check-niri-isolated.py`：
GTK/Qt/foot/Chromium各16项、共64项通过，活动组合动态缩放后候选与提交正确。
它在私有无屏幕Sway内运行系统niri，单输出验证1×/1.25×/1.5×/2×，不操作当前
桌面；不能代替实体niri双屏。测试路径与边界见[真实应用说明](REAL-APP-HARNESS.zh-CN.md)。
