# GTK 原生候选窗接入

`fcitx5-gtk-tipe.patch` 基于 fcitx5-gtk 5.1.7，固定提交
`3b18d2ab7401d4233daf38bbc5896f1703685a43`。GTK3 与 GTK4 共用
`gtk3/inputwindow.cpp`，因此两者接入同一个 TiPE painter。

只有 `CurrentIM` 信号中的唯一名等于 `tipe` 时，尺寸测量、绘制、鼠标命中与
悬停使用项目的 `client_side_candidate_adapter` / `candidate_painter`。
其他输入法继续使用上游主题布局。GTK 仍负责 popup 的父窗口、光标定位、
Wayland 生命周期和设备缩放；此补丁不修改 `ClientSideInputPanel` 能力。

## 构建与测试

```sh
./integration/build-gtk-painter.sh
```

需要 C++20 编译器、CMake、ECM、pkg-config、GTK3 或 GTK4 开发包，以及
GLib、Pango/Cairo、xkbcommon 开发包。脚本自动选择本机可用的 GTK 开发包；
可设置 `TIPE_BUILD_GTK3=ON` / `TIPE_BUILD_GTK4=ON` 强制要求某一版本。
默认构建目录是 `$XDG_CACHE_HOME/tipe/fcitx5-gtk/build`，若未设置
`XDG_CACHE_HOME` 则使用 `$HOME/.cache`。

脚本固定上游提交并校验补丁是否已应用，不会 reset 已有源码目录。可以用
`TIPE_GTK_SOURCE_DIR`、`TIPE_GTK_BUILD_DIR`、`TIPE_BUILD_JOBS` 覆盖构建位置和并行度。

构建会运行 `tipe-gtk-candidate-smoke`，直接向实际 GTK InputWindow 发出
GObject 信号，并验证输入法识别、单行/展开模式、鼠标候选索引、选中色、
更新后悬停状态与空候选隐藏。该测试不创建窗口、不注入键盘、不连接用户的
Fcitx 服务；它不能代替真实 Wayland 桌面的 popup 定位检查。

## 独立用户目录安装

```sh
TIPE_GTK_PREFIX="$HOME/.local/share/tipe/gtk" ./integration/build-gtk-painter.sh --install
```

安装仅写入所给 prefix。GTK4 模块位于
`lib/gtk-4.0/4.0.0/immodules/libim-fcitx5.so`；GTK3 模块位于
`lib/gtk-3.0/3.0.0/immodules/im-fcitx5.so`。实际 ABI 目录由本机 GTK
pkg-config 提供。脚本不会修改系统库、登录环境或当前应用进程；已经启动的
应用仍使用它原来加载的模块。验收时应由显式设置模块搜索路径的新应用加载，
并检查 `/proc/<pid>/maps` 确认使用了该 prefix 的模块。

构建脚本同时应用独立的 `fcitx5-gtk4-popup.patch`。GTK4 的 `GtkText` 默认
把整个控件高度作为输入法光标矩形；当单行输入框被拉伸到接近屏幕高度时，
候选窗会被锚定到控件底部。GTK 4.22 的 popover 随后可能收到小于其最小尺寸的
合成器分配，并立即关闭。补丁在 GTK 4.4 及以上使用
`gtk_text_compute_cursor_extents()` 的真实文字行高度和纵坐标，保留客户端传入的
横坐标，以继续跟随预编辑光标、双向文本和水平滚动。其他控件沿用其原有矩形。
修复只使用控件本地坐标，不推算全局屏幕坐标。

GTK4 补丁还在候选更新后主动请求重绘，确保候选宽度不变时，高亮和候选内容也会
更新。真实桌面回归应同时覆盖正常高度与拉伸后的 `GtkEntry`，检查候选窗可见且
贴近文字行，并用方向键检查相同尺寸下的高亮移动。

GTK4 的弹窗还监听父 `GdkSurface` 的 `enter-monitor`、`leave-monitor` 和
缩放属性通知。跨屏时先隐藏旧 popup，再合并通知、重建 popup，让合成器重新
选择输出约束和 buffer scale；父窗口、输入上下文和未提交拼音不变。关闭输入
上下文时取消待处理的重建并断开 surface 信号，不增加轮询或全局坐标推算。

可运行 `python3 scripts/check-multioutput-desktop.py` 检查真实 GTK/Qt 模块在
隔离双屏 Mutter 下的跨屏组合、展开宽度及鼠标命中。该检查使用独立 D-Bus、
HOME、学习目录和虚拟输入设备，不更改当前桌面输出或读取实体键盘。
