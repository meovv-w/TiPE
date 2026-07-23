# 排查与调试

本文按“先确认运行状态，再缩小到状态机、UI 或模型”的顺序排查 TiPE。

## 最先运行的命令

```bash
fcitx5-remote -n
tipe-toggle --status
tipe-doctor
```

重点确认：

- 当前输入法是否为 `tipe`。
- TiPE 内部模式是中文还是英文。
- 引擎 addon 和 UI addon 是否都已加载。
- 当前 fcitx5 进程是否使用了预期模型配置。
- 监督快照、用户词典和偏好文件是否可读。

`tipe-doctor` 默认只读，不会修复文件、重启 fcitx5 或切换输入法。

不访问运行中 fcitx5：

```bash
tipe-doctor --no-runtime
```

## 构建问题

完整构建：

```bash
./scripts/build.sh
```

只重跑 CTest：

```bash
ctest --test-dir build --output-on-failure
```

完整回归：

```bash
./scripts/smoke-test.sh
```

内存和未定义行为检查：

```bash
./scripts/build.sh --sanitize
ctest --test-dir build-sanitize --output-on-failure
```

HTTP 模型测试在某些沙箱中不能创建本机回环 socket，会以 CTest skip 状态退出。可在普通终端单独运行：

```bash
python3 tests/model_backend_http_test.py scripts/model-adapter.sh
```

## 安装内容不一致

先检查预览：

```bash
./scripts/install.sh --dry-run
./scripts/uninstall.sh --dry-run
```

安装脚本要求 `build/.tipe-build-tested` 指纹与当前全部构建输入一致。源码或脚本在最后一次测试后发生变化时，需要重新运行 `./scripts/build.sh`。

查看 CMake 实际安装清单：

```bash
sed -n '1,240p' build/install_manifest.txt
```

TiPE 不会自动修改 fcitx5 profile。addon 已加载但不能切换时，先确认 profile 是否包含 `Name=tipe`，再由用户决定是否修改。

## 中文/英文切换

```bash
tipe-toggle --status
tipe-toggle --set chinese
tipe-toggle --set english
```

模式请求通常位于：

```text
$XDG_RUNTIME_DIR/tipe/input-mode
$XDG_RUNTIME_DIR/tipe/input-mode-applied
```

请求文件和已应用文件应具有相同模式及唯一 token。`tipe-toggle` 只有在引擎确认同一 token 后才报告成功。

如果桌面快捷键调用 `fcitx5-remote -t`，它会停用 TiPE，英文按键不会进入监督。快捷键应调用：

```text
$HOME/.local/bin/tipe-toggle
```

## 不切换输入法重放问题

### 查看候选

```bash
./build/tipe-state-probe nihao
./build/tipe-state-probe nihao --move Down
```

### 检查选短词后剩余拼音

```bash
printf '%s\n' \
  'type nihao' \
  'select 你' \
  'expect-preedit hao' |
  ./build/tipe-state-probe "" --script -
```

### 检查候选格和数字

```bash
./build/tipe-state-probe nihaoshijie \
  --move Down \
  --snapshot 100,200,3,18
```

探针输出中的关键行：

| 行 | 含义 |
| --- | --- |
| `preedit` | 当前剩余拼音 |
| `preedit_cursor` | 拼音编辑光标 |
| `selected` | 当前候选索引 |
| `visible` | 当前带数字候选 |
| `candidate-meta` | 候选来源和消耗拼音长度 |
| `events` | 按键类型统计 |
| `snapshot` | 候选窗口实际协议 |

长问题应写成脚本文件，每行一个动作。支持的常见动作：

```text
type nihao
select 你
move Down
key ISO_Left_Tab
digit 2
space
backspace
delete
escape
expect-preedit hao
expect-candidate 0 好
expect-expanded 1
```

## 候选排序异常

查看当前学习：

```bash
tipe-check-preferences --summary --top 20
tipe-check-preferences --explain
tipe-check-preferences --preedit nihao
```

候选偏好和原样英文使用不同阈值。只有一次的记录可能显示为证据，但尚未激活，这是正常保护，不代表文件没有更新。

查看用户词典：

```bash
tipe-check-user-dictionary --explain
```

分段生成的新词通常需要两次一致完整链路才永久写入。

状态探针默认不读取真实用户数据。要确认当前用户学习是否影响结果：

```bash
./build/tipe-state-probe nihao --user-data
```

## 监督快照

默认目录：

```text
~/.cache/tipe/
```

主要文件：

- `supervision-current.tsv`：当前活动请求。
- `supervision-last.tsv`：最近完成请求。
- `supervision-history.tsv`：一般有界历史。
- `supervision-training-history.tsv`：终止训练记录。

查看统计而不输出样本内容：

```bash
tipe-training-export --stats
```

查看最近少量标注样本：

```bash
tipe-training-export --limit 5
```

密码框中不应生成当前快照、历史、状态弹窗或模型调用。发现密码字段仍有记录时，应立即停止测试并报告具体应用与前端类型。

## 光标跟随与 UI

精确候选位置依赖客户端和输入协议提供的光标矩形。先用调试重启收集一次日志：

```bash
TIPE_DEBUG=1 ./scripts/restart-fcitx5.sh
```

调试完成后应恢复普通重启，避免持续写日志：

```bash
./scripts/restart-fcitx5.sh
```

日志通常位于 `~/.cache/tipe`，用户服务启动时也要查看：

```bash
journalctl --user -u fcitx5.service
```

### Wayland input-method-v2

有效路径依赖 compositor 的 `text_input_rectangle`。日志中应看到：

```text
popup	text-rect
popup	rendered
```

窗口超出屏幕时检查 `boundsOk`。输入上下文切换后，旧 popup surface 必须失效并等待新矩形，不能继续使用上一个文本框的位置。

### 浏览器和 GTK

部分浏览器声明 `ClientSideInputPanel`。TiPE 激活自己的 UI 后，日志应连续出现：

```text
ui-route direct frontend=dbus
tipeui update frontend=dbus
```

只有第一行表示引擎选择了直连路径，但 UI addon 没有收到更新。

浏览器中的 preedit 由客户端渲染，候选由 TiPE UI 渲染；二者不要重复绘制。

### XIM

XIM 常见矩形高度和宽度为零，只要 x/y 有效，TiPE 会把它视为插入点。四个值全为零时才进入后备路径。

HiDPI Xwayland 的根窗口坐标可能是物理像素，而 GTK 使用逻辑像素。日志中的 frontend scale 和 monitor geometry 必须一起判断，不能直接比较原始数值。

### Wine

Wine XIM 可能一直返回 `0,0,0,0`。TiPE 会按需启动 `tipe-wine-caret-bridge.exe`，通过标准 MSAA `OBJID_CARET` 获取插入点，并只额外判断控件是否具有 IMM context。

调试日志中的有效后备行：

```text
fallback	wine-caret
```

控件支持 IMM 时，由控件绘制 preedit；不支持时，TiPE 使用透明、不可聚焦的覆盖层绘制拼音。覆盖层没有输入区域，不应抢占鼠标和键盘。

MSAA 也没有光标时，TiPE 只能保持稳定的 X11 指针后备，无法从输入法一侧推导任意应用内部光标。

## 候选窗口闪烁

重点检查：

- 是否每次更新都创建了新窗口而不是更新现有 surface。
- input context serial 是否变化。
- 空更新是否来自旧输入上下文。
- 模型请求时是否先显示临时候选顺序又立即恢复。

TiPE 的模型路径应在请求开始时只展开当前顺序，等待工作线程返回后一次更新最终顺序。快速模型也不应出现“重排后闪回”。

## 模型问题

查看配置：

```bash
tipe-model-config --show
tipe-model-current --show
```

当前模型自测：

```bash
tipe-model-self-test --current
```

不访问服务的请求自测：

```bash
tipe-model-self-test --current --adapter-dry-run
```

模型配置保存后，学习窗口可以立即用于下一次手动分析；运行中的 fcitx5 需要明确重启后才继承新的 `TIPE_MODEL_COMMAND`。

API Key 的诊断输出只应显示来源和 set/unset，不应显示值。Key 文件应满足：

```bash
stat -c '%a %n' ~/.config/tipe/model-api-key
```

预期权限为 `600`。

## TiP 训练失败

先看数据量：

```bash
tipe-training-export --stats
```

手动训练并保留输出：

```bash
tipe-personal-model-train
```

检查当前模型：

```bash
tipe-personal-model inspect
```

“保留现有模型”不等于训练进程失败。验证收益不足、通用排序不安全或新模型会失去已经激活的能力时，训练会成功结束但拒绝替换现有模型。

运行时蒸馏失败会重试三次。即使蒸馏暂时失败，新 TiP 文件仍可有效发布，旧运行时规则会保留到下次重试。

## 崩溃

查看当前启动后的 coredump：

```bash
coredumpctl list fcitx5
coredumpctl info fcitx5
```

带符号回溯：

```bash
coredumpctl debug fcitx5
```

同时保留：

- 崩溃前输入的最短拼音。
- 使用的前端类型：Wayland v2、DBus/GTK、XIM 或 Wine。
- 是否展开候选。
- 最后一个方向键或数字键。
- `tipe-doctor` 输出。
- 对应的状态探针脚本。

不要只提交“随便乱打会崩”。能在 `tipe-state-probe --script` 中复现的动作序列最容易修复并加入回归测试。

## 报告问题建议格式

```text
应用：
桌面环境 / compositor：
前端类型：
缩放比例：
输入拼音：
按键步骤：
预期结果：
实际结果：
是否可由 tipe-state-probe 复现：
tipe-doctor 关键输出：
```

提交前请移除 API Key、私人周边文字和不希望公开的监督样本。
