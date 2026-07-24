# 安装与使用

本文说明如何在当前 Linux 用户下构建、安装和试用 TiPE。所有安装命令都以项目根目录为工作目录。

## 支持环境

TiPE 当前主要在以下组合上开发和验证：

- Linux
- fcitx5
- Wayland；同时保留 DBus/GTK 和 XIM 后备路径
- CMake 3.20 以上
- C++20 编译器

构建依赖包括：

- fcitx5 Core、Utils 和 Wayland 开发文件
- Wayland client 与 `wayland-scanner`
- GTK4 与 gtk4-layer-shell
- PangoCairo
- XCB
- Python 3
- 推荐：LibIME Pinyin
- 可选：libpinyin
- 可选：`x86_64-w64-mingw32-g++`，用于构建 Wine 光标桥

不同发行版的软件包名称不同。CMake 缺少依赖时会直接报告对应的模块名。

## 获取源码

```bash
git clone https://github.com/meovv-w/TiPE.git
cd TiPE
```

## 构建

```bash
./scripts/build.sh
```

这个脚本会：

1. 以当前用户 `~/.local` 为安装前缀配置 CMake。
2. 默认生成 `RelWithDebInfo` 构建。
3. 最多使用四个并行任务，并以 `nice 5` 降低构建优先级。
4. 编译全部目标。
5. 运行 CTest。
6. 为所有构建输入写入测试指纹。

需要调整时可显式设置：

```bash
TIPE_BUILD_JOBS=8 TIPE_BUILD_NICE=10 ./scripts/build.sh
```

使用 ASan 和 UBSan：

```bash
./scripts/build.sh --sanitize
```

完整回归检查：

```bash
./scripts/smoke-test.sh
```

`smoke-test.sh` 会在临时 HOME 中测试真实安装和卸载，不会替换当前用户正在使用的输入法文件。

## 在不切换输入法的情况下检查

状态探针直接调用同一套 C++ 状态机：

```bash
./build/tipe-state-probe nihao
./build/tipe-state-probe nihao --move Down
./build/tipe-state-probe nihao --digit 2
./build/tipe-state-probe jixuzuo --select 继续
```

查看剩余拼音：

```bash
printf '%s\n' \
  'type nihao' \
  'select 你' \
  'expect-preedit hao' |
  ./build/tipe-state-probe "" --script -
```

探针默认使用临时学习文件，不会污染真实候选排序。只有显式加入 `--user-data` 时才读取当前用户数据。

## 安装到当前用户

先查看将要安装的文件：

```bash
./scripts/install.sh --dry-run
```

确认后安装：

```bash
./scripts/install.sh
```

安装脚本只写 `~/.local`。主要位置包括：

| 内容 | 路径 |
| --- | --- |
| fcitx5 引擎 | `~/.local/lib64/fcitx5/libtipe.so` 和兼容副本 |
| TiPE UI | `~/.local/lib64/fcitx5/libtipeui.so` 和兼容副本 |
| addon 元数据 | `~/.local/share/fcitx5/addon/` |
| 输入法元数据 | `~/.local/share/fcitx5/inputmethod/tipe.conf` |
| 命令行工具 | `~/.local/bin/tipe-*` |
| 桌面入口 | `~/.local/share/applications/tipe-supervision.desktop` |
| 图标和支持页资源 | `~/.local/share/icons`、`~/.local/share/tipe/support` |

安装过程先写入私有临时目录并验证完整性，再逐个原子替换目标文件。构建输出或测试指纹过期时，安装会拒绝继续。

## 激活 TiPE

安装不会：

- 修改 fcitx5 profile。
- 修改系统输入法配置。
- 修改 niri 或其他桌面快捷键。
- 重启 fcitx5。
- 切换当前输入法。

确认愿意改变当前输入会话后，先预览：

```bash
./scripts/restart-fcitx5.sh --dry-run
```

再执行：

```bash
./scripts/restart-fcitx5.sh
fcitx5-remote -n
```

正常结果应为：

```text
tipe
```

如果 addon 已加载，但 `fcitx5-remote -s tipe` 不能选中 TiPE，通常表示当前 fcitx5 profile 没有 TiPE。是否修改 profile 属于用户输入环境决策，TiPE 不会自动处理。

## 中文与英文模式

TiPE 内部模式切换不会停用 fcitx5：

```bash
tipe-toggle
tipe-toggle --status
tipe-toggle --set chinese
tipe-toggle --set english
```

行为规则：

- 当前不是 TiPE 时，普通 `tipe-toggle` 会先激活 TiPE 中文模式。
- 中文模式有活动拼音时切到英文，拼音会暂存。
- 暂存拼音会继续显示在 TiPE 面板中，右侧标记 `Eng`；此时没有可误选的中文候选。
- 立刻切回中文会恢复拼音。
- 英文模式输入第一个可打印字符时，暂存拼音会先按原样提交。
- 英文模式的普通字符直接进入应用，同时形成有界行为记录。
- 切换结果由 TiPE 自己显示短暂 `TiPE` 或 `Eng` 标识。

桌面快捷键应调用 `tipe-toggle`，不要直接调用 `fcitx5-remote -t` 或 `-c`。后两者会停用引擎，使英文按键完全绕过 TiPE。

## 候选窗口

### 收起状态

- 默认只显示一行候选。
- `Right` 向右浏览候选。
- `Left` 先向左浏览；回到第一项后继续按左键会移动拼音编辑光标。
- 数字键只选择当前显示且标有该数字的候选。

### 展开状态

- `Down` 展开候选网格。
- `Down / Up` 按行移动。
- `Left / Right` 在网格中移动。
- `PageDown / PageUp` 移动一整行。
- `Home / End` 跳到第一项或最后一项。
- `Tab / Shift+Tab` 前后移动一项。
- 鼠标悬停高亮，单击使用与键盘相同的选择和学习路径。

长候选会占用多个格宽，后续短候选会回填仍放得下的空位。窗口会根据可用屏幕边缘向上、向下或横向调整。

## 长拼音和分段选词

TiPE 允许一段拼音分多次选择：

```text
nihao
选择“你”
剩余 hao
选择“好”
```

同样支持多字前缀：

```text
jixuzuo
选择“继续”
剩余 zuo
```

完整且重复确认的分段链可以形成用户新词。第一次链路通常只作为证据，第二次一致确认才会写入用户词典，降低误操作污染。

## 中英文混输

已知或学到的英文标识符可以原样显示：

```text
github
docker
qwen2
```

也可以与中文后缀组合：

```text
woxiangyongdocker -> 我想用docker
dagithubdeshihou -> 打github的时候
```

活动小写拼音中输入大写字母会切换为保留大小写的原样英文组合，避免把大写字母单独提交后丢失前面的内容。

## 学习窗口

打开：

```bash
tipe-supervision-window
```

学习窗口不会因为打开或刷新就调用模型。

- “学习”页显示自动词序、可训练记录和已经激活的个人习惯。
- “模型”页选择模型、保存 API 配置和执行一次分析。
- “支持”页显示项目支持码。
- “高级详情”保留开发诊断字段，普通使用不需要展开。

## 诊断

基础检查：

```bash
tipe-doctor
tipe-toggle --status
fcitx5-remote -n
```

检查学习文件：

```bash
tipe-check-preferences --summary --top 10
tipe-check-user-dictionary --explain
tipe-training-export --stats
```

更多命令和常见问题见 [排查与调试](debugging.md)。

## 卸载

先查看：

```bash
./scripts/uninstall.sh --dry-run
```

确认后删除 TiPE 管理的当前用户文件：

```bash
./scripts/uninstall.sh
```

卸载脚本不会修改：

- fcitx5 profile。
- niri 或其他桌面快捷键。
- 系统输入法。
- 用户学习数据目录。

如果桌面快捷键仍调用 `tipe-toggle`，卸载预览会发出提醒。是否删除个人学习数据应由用户单独决定。
