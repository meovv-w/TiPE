# 真实应用最小 harness

当前桌面编辑矩阵入口是 `scripts/check-editing-desktop.py`。它顺序打开自有
GTK、Qt、原生 Wayland Chromium、Wine EDIT 与 foot 窗口，只对匹配进程或
新建窗口 ID 的聚焦窗口发送物理按键；失去焦点立即停止，结束后恢复原焦点。
GTK/Qt 还核对用户实际安装模块的路径和 inode，避免只测到 staging 插件。

仅回归超过64字符的组合，可运行：

```bash
TIPE_SEGMENT_CASE='Long composition crosses' TIPE_WINE_INPUT_STYLE=overthespot \
  python3 scripts/check-editing-desktop.py --matrix segmented \
  --output build/long-window-desktop gtk qt browser wine foot
```

该项输入68字符，真实选中前60字符对应的中文，检查剩余`zhongguo`及Shift往返，
再提交“中国”。GTK/Qt/浏览器读回客户端文字和preedit；Wine读回已提交文字，
foot读回最终行，不将缺失的协议能力伪装成已观察。

```bash
python3 scripts/check-editing-desktop.py gtk qt browser
python3 scripts/check-editing-desktop.py --output build/editing-protocol-regression wine foot
python3 scripts/check-editing-desktop.py --matrix segmented gtk qt browser wine foot
python3 scripts/check-editing-desktop.py --matrix chain-learning gtk qt
```

矩阵直接断言控件最终文字，覆盖 Enter/Space/Escape、拼音中间插入和删除、
Shift 往返的插入位置、展开收起、方向键、翻页、数字选择、长拼写、客户端
选区替换和光标插入、提交恢复及取消后的新组合。Wine 使用独立 WINEPREFIX，
文本由 Win32 EDIT 直接写文件；XIM 无周围文本能力时只要求正常客户端删除。
foot 运行 Python 行读取器，输入永远是数据，不运行 shell；只验证组合编辑，
不要求终端具备 GUI 选区或周围文本恢复。脚本不清除个人学习数据，不重启
daemon，不修改 niri 配置；所有桌面脚本必须顺序运行，不能并发抢焦点。
先等待完整CTest结束，再运行桌面矩阵；其中的隔离应用探针也不应与实际桌面
按键检查交错运行。

结果在各自输出目录的 `results.json`；GTK/Qt 保留 JSONL，浏览器结果包含
DOM composition/input 事件。此矩阵不覆盖任意分段选择链、所有屏幕边缘、
多显示器或登录后的聊天界面，当前结论见 [验收表](ACCEPTANCE.md)。

`segmented` 矩阵逐步核对选择后的客户端文字和剩余拼音，包括前缀数字选择、
编辑/取消剩余拼音、Shift往返、`woc` 分选“我”“操”、连续三个词组，以及
删空剩余拼音后恢复前一个汉字。候选索引由隔离 `tipe-demo --json-lines
--state-dir <临时目录>` 查询，最终成败来自真实控件的固定文字/拼音断言。
Wine只观察已提交文字，foot只观察最后交付的完整行，不冒充中间preedit证据。

`chain-learning` 为GTK/Qt创建唯一应用标识，手动完成三次 `woc` 选词链并确认
文字保留；随后检查后台发布的规范拼音和缩写证据，再直接输入 `woc` 加空格，
要求自动得到“我操”，不重启输入法、不点击更新、不清空任何个人模型。

原生Wayland候选窗四角检查入口：

```bash
python3 scripts/check-native-popup-desktop.py browser foot
```

此脚本要求当前只有一块位于原点的输出。顺序创建独立Chromium输入框和foot
原始输入读取器，移动自有窗口到四角，每角检查普通/长句的紧凑及展开窗。
截图直接检查可见边界和宽度，客户端Wayland日志提供实际光标矩形，键盘和
鼠标分别选择首项并检查固定预期文字。终端输入只交给Python读取器，不执行
shell。结束后关闭自有窗口、清理浏览器配置并恢复原焦点。结果默认写入
`build/native-popup-regression/results.json`，每次运行覆盖，因此分开执行时
应先保存各自结果。本轮两类前端16项通过，证据见`native-popup-results.json`；
多屏和混合缩放不属于此脚本覆盖范围。

另有隔离多屏检查：

```bash
python3 scripts/check-multioutput-desktop.py
```

需要 Mutter 18 的 GI typelib、PyGObject、Pillow 和 NumPy。脚本创建私有
D-Bus、HOME、XDG 目录和两个 headless 输出，用实际已安装 GTK/Qt 模块与独立
Fcitx 进程输入。通过 Mutter 虚拟 seat 发送按键和指针，检查焦点进程身份；
不调用 niri、不注入当前桌面或修改实体输出。运行时从 framebuffer 读取真正
渲染的输出画面，并对照客户端 xdg_popup anchor 检查候选定位。

覆盖 1× 与 2×、1.25×、1.5× 双向跨屏：普通词和长句的组合未提交时移动，
检查紧凑/展开宽度、可见边界、移回后的展开窗，以及最终键盘/鼠标提交。
`runtime.json` 记录每个夹具实际映射模块的路径、inode 和 SHA256；仅当全套
结束才写 `complete:true`。输出默认位于 `build/multioutput-regression`，不会
把隔离 Mutter 结果当作实体多屏 niri 或 XIM 的验证。

原生input-method-v2双屏入口：

```bash
python3 scripts/check-native-multioutput.py
python3 scripts/check-native-multioutput.py --frontend wine
```

需要Sway/wlroots、foot、wtype、grim及Pillow/NumPy。本机开发依赖已解包到
`build/sway-private/root`；其他位置可用`TIPE_TEST_SWAY_ROOT`指定，无需替换系统
compositor。脚本单独创建D-Bus、HOME/XDG与headless输出，加载当前安装的三个
TiPE/Fcitx模块并核对inode。私有虚拟键盘先提供有效keymap，之后每次输入检查
自有foot焦点；与niri桌面绕过抓键的虚拟键盘行为不同，这里的实际中文提交
可由Python原始stdin读取器确认。原生popup定位来自协议，Sway IPC只移动测试窗口。

普通/长拼写、键盘/鼠标、三种缩放共12项，保留活动组合双向跨屏，直接从各输出
截图检查完整边界、逻辑宽度和客户端光标矩形。结果位于
`build/native-multioutput-regression`，本轮12项通过。Sway 1.11未向input-method
转发Chromium首次普通字段延迟声明的用途，TiPE按未知用途保护性回退；因此该组
只覆盖foot，不把它计为Chromium或实体niri多屏验证。

`--frontend wine`在同一私有compositor启动独立Xwayland和临时Wine环境，
使用标准overthespot样式。XTest按X11当前keymap发送输入，逐键确认焦点属于
自有Win32窗口；不把wlroots虚拟键盘的固定keycode解释当作X11输入证据。
通过Win32 GetGUIThreadInfo/ClientToScreen取得独立caret位置，直接读取实际
EDIT文字。移屏用同一工作区的绝对移动，避免测试命令自行造成失焦提交。
截图失败会保存X11窗口树、输入上下文、drawable及一次重绘后的画面用于诊断，
该诊断不会把失败改成通过。输出位于`build/xim-multioutput-regression`。

Wine标准光标跟随样式、按应用配置和恢复的检查入口见[Wine说明](WINE.zh-CN.md)。

已安装niri的隔离检查入口：

```bash
python3 scripts/check-niri-isolated.py
```

需要已安装niri、Sway/wlroots、GTK/Qt夹具、foot、Chromium、grim及编译依赖
wayland-client/xkbcommon。它在私有无屏幕Sway内运行未修改的niri程序，再在
niri内启动独立Fcitx与测试客户端。两层窗口均核对焦点和进程身份，所有输出、
D-Bus、HOME与XDG目录都属于临时会话；不操作当前桌面、系统输入设备或个人模型。

外层虚拟键盘使用标准evdev/US映射，让Winit收到真实键码并经过niri的输入法
抓键路径。不能直接在内层调用wtype；这会绕过需要验证的抓键。也不能把wtype
动态分配的外层键码当成标准键盘，它们经过Winit会变成不同字符。

默认顺序验证GTK、Qt、foot和Chromium，普通/长词各用键盘及鼠标选词。
在同一段活动组合期间将niri输出从1×改为1×/2×/1.25×/1.5×，读取实际画面、
客户端协议光标/anchor矩形与最终文字，要求缩放及展开前后逻辑宽度保持。
`--frontends foot browser --scales 1.25 2`可缩小范围。结果和安装模块映射在
`build/niri-isolated-regression`，只有整套结束后`runtime.json`才写`complete:true`。
该测试是单输出动态缩放，不代替实体niri双屏或跨输出迁移证据。

观察未修改的Discord真实界面，可运行：

```bash
python3 scripts/check-niri-isolated.py --probe-discord
```

该模式同样在私有会话中启动已安装Discord，使用全新的临时配置，不复制登录
资料，不替换页面DOM，不填写任何字段或发送消息。每5秒只读取页面就绪状态、
公开页面文字和输入框的类型/名称，并记录截图；默认18次，可用
`--discord-probe-samples 3`缩短诊断。结果位于`build/discord-native-probe`。
`probe_complete:true`只表示观察结束，`discord_real_ui_ready`表示是否出现真实
网站输入框；二者都不代表聊天输入通过，常规`complete`仍保持false。

2026-10-23的完整观察始终停在`Discord Updater / Checking for updates`，没有
输入框；更新服务连接失败（curl退出7）。因此保留原先四项Electron启动器
渲染器输入证据，但不能把它提升为真实登录聊天验收。截图和逐次结果见
`docs/evidence/2026-10-23/discord-native-*`。

以下是早期最小探针的操作方式与历史记录。

工作区提供两个只用于观察的真实 toolkit 窗口：`tipe-gtk-harness`（GTK 4）和
`tipe-qt-harness`（Qt 6）。它们通过正常的 GTK/Qt 输入法上下文接收事件，并把
toolkit 侧看到的 `preedit`、`commit`、`delete_surrounding`、`input_method_query`
以及文本矩形写成 JSONL。程序不会链接 TiPE 核心，不会伪造 Fcitx 回调，也不会
修改 profile、切换输入法或重启 fcitx5。

构建后运行：

```bash
TIPE_HARNESS_TIMEOUT_MS=7000 \
  ./scripts/run-real-app-harness.sh build build/real-app-harness
```

脚本先记录当前 `DISPLAY`/`WAYLAND_DISPLAY`、fcitx5 版本、当前输入法、GTK 3/4、
Qt 6 和 `foot` 版本，然后分别打开两个窗口。结果在
`build/real-app-harness/run.log`、`gtk4.jsonl` 和 `qt6.jsonl`。如果当前输入法不
是 TiPE，窗口仍可用于检查 toolkit 的上下文和光标矩形，但日志会明确写出
`tipe_observation=not-active`，不能把这次运行当作 TiPE 的 preedit/commit 证据。

在 X11/XWayland 窗口可被安全发现时，可显式开启短输入序列：

```bash
TIPE_REAL_APP_AUTOTYPE=1 ./scripts/run-real-app-harness.sh build build/real-app-harness
```

该选项只在已有 `DISPLAY` 和 `xdotool` 时尝试向 harness 自己的窗口发送
`nihao`、空格；Wayland 原生窗口没有安全的 xdotool 路径，脚本会记录跳过原因。
脚本不会为了测试而切换全局输入法。要观察 TiPE，用户需先在当前会话手动选择
TiPE，并确认插件已经由该 fcitx5 会话加载。

如需验证真实编辑光标路径，可设置 `TIPE_REAL_APP_SCENARIO=edit`。脚本会在每个
GTK/Qt 窗口中执行 `nihao`、`shijie` 两次提交，移动客户端光标、Backspace 删除，
再输入 `github` 和 `nihao`；GTK/Qt harness 会记录文本变化与光标位置。这是实际
toolkit 输入上下文的观察日志，仍不会把日志中没有出现的视觉或 Wayland popup 行为
推断为已通过。

也可以直接运行 `./scripts/run-real-app-edit-regression.sh build <输出目录>`，它会
在隔离会话中执行该序列，并检查 GTK4/Qt6 最终都观察到 `你好界github你好`。

GTK4/Qt6 harness 支持 `TIPE_HARNESS_NO_SURROUNDING=1`，会让真实输入控件对
surrounding-text 查询返回不可用值，同时保留正常输入和提交。这样可以单独观察
客户端无法提供 surrounding-text 时的 fail-closed fallback：

```bash
TIPE_HARNESS_NO_SURROUNDING=1 \
  ./scripts/run-isolated-real-app-harness.sh build build/isolated-no-surrounding
```

输出中的 GTK4/Qt6 `ready`/查询事件会记录
`surrounding_available:false`；该场景不会声称完成跨应用视觉验收。

密码/敏感字段可以显式运行一次探测。设置 `TIPE_HARNESS_PASSWORD_ECHO=1`（也
接受 `TIPE_HARNESS_PASSWORD=1`）后，GTK 使用 `GTK_INPUT_PURPOSE_PASSWORD`、
私密输入提示和隐藏回显，Qt 使用 `QLineEdit::Password` 及 `ImhSensitiveData`。
JSONL 的 ready 事件记录 `sensitive`、输入目的和回显模式；所有 preedit、commit、
文本和 surrounding text 只记录长度并标为 `redacted`，不会把密码写入日志。普通
模式保留原来的文本证据。

若不希望触碰当前桌面 profile，可以使用隔离脚本：

```bash
./scripts/run-isolated-real-app-harness.sh build build/isolated-real-app-harness
```

该脚本创建临时 XDG 配置和 `DefaultIM=tipe` profile，在独立 `dbus-run-session` 中启动私有 fcitx5，并优先使用 Xvfb；本机没有 Xvfb 时会使用连接当前 Wayland compositor 的嵌套 Xwayland。它等待私有 daemon 加载 `tipe` addon 后再启动 GTK4/Qt6 窗口，点击窗口取得焦点，发送 `nihao` 加空格，并要求两个 JSONL 均出现 `你好` commit。输出还保留私有 daemon 日志和临时 profile 路径；脚本不会修改用户 profile、重启现有 fcitx5 或改变桌面 keybinds。退出时会删除临时根目录及 Chromium/Wine 缓存，重复运行不会在 `/tmp` 累积数据。

隔离脚本的默认运行还会以临时 XDG 状态目录和 HOME 启动私有 daemon；它随后用密码
提示再次运行 GTK4/Qt6，验证 capability、隐藏回显和日志脱敏，并比较私有
`model.current` 摘要确认敏感输入没有进入学习模型。

隔离脚本还会在同一临时 X11 会话中探测 Chromium（`chromium-browser`、`chromium` 或
`google-chrome`）的独立输入框，并在输出目录写入 `chromium-smoke.log`；页面标题会记录
是否观察到 `你好`。Chromium 的渲染器在不同发行版可能不启用 X11 IME，因此
`commit-not-observed` 只表示这次浏览器探测没有取得提交证据，不会伪装成成功。若主机安装
Mutter，可设置 `TIPE_ISOLATED_X11_WM=1` 让隔离脚本使用 Mutter headless + Xwayland
提供 EWMH 焦点管理；Chromium 同时尝试 XSendEvent 和 XTEST 按键路径，并将 renderer
keydown 计数写入窗口标题，仍以页面内提交文字作为唯一成功证据。可设置
`TIPE_REAL_APP_STRICT_APPS=1`，让已找到窗口但未观察到提交时使脚本失败。Wayland 原生
`foot` 也会写入 `foot-smoke.log`；隔离脚本使用嵌套 Xwayland 并主动清除
`WAYLAND_DISPLAY`，所以会明确记录 `no-Wayland-display-for-foot`，不会把 X11 窗口冒充
Wayland 终端验收。

如果主机安装了 Wine，隔离脚本还会在临时 `WINEPREFIX` 中启动 Wine 内置
`notepad.exe`，写入 `wine-smoke.log`。它通过同一 X11/Fcitx 路径发送 `nihao` 和空格，
再尝试从嵌套 Mutter 的 `wayland-0` 剪贴板读回全文；只有读回精确的 `你好` 才记录
`status=commit-ok`。Wine 窗口标题或按键注入本身不算提交证据；没有可读剪贴板、焦点
管理器或 Notepad 窗口时，会记录 `commit-not-observed` 或具体的
`status=skipped reason=...`，并保留 `wine-notepad.stderr`/可选截图供人工诊断。该探针
不会复用 `$HOME/.wine`，也不会修改现有 Wine 前缀；`TIPE_REAL_APP_STRICT_APPS=1`
可将已找到窗口但无法读回文本的结果设为脚本失败。

本机一次窄范围运行（2026-09-09）记录到 GTK 4.22.4、Qt 6.11.1、foot 1.27.0，
会话为 niri/Wayland（同时有 `DISPLAY=:0`）。当前选择为 `keyboard-us`，因此只
取得了 toolkit 的 ready、文本矩形和 Qt 光标查询记录；没有声称 GTK/Qt/foot、浏览
器、Discord、Wine 或 Wayland popup 的完整视觉和交互验收。真实应用清单仍需在
TiPE 已启用的用户桌面逐项观察。当前环境虽安装 Wine 11.0 (Staging)，但嵌套
Xwayland/Mutter 没有提供稳定的 Wine Notepad 文本读回证据；最近运行的
`wine-smoke.log` 因而只能作为 `commit-not-observed`/诊断记录，不能写成 Wine 已通过。
2026-09-10 的隔离运行实际发现了 `Untitled - Notepad` 窗口并完成了按键注入，日志为
`status=commit-not-observed ... backend=wine-x11 observation=unavailable`，同时记录
`clipboard_bytes=0` 和窗口管理器不支持 `_NET_ACTIVE_WINDOW`；这正是该探针应保留的
失败证据。
