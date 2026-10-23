# TiPE 原生候选窗集成

GTK/Qt 应用内候选窗与 Fcitx classicui 的 TiPE 候选使用项目中的同一个
`candidate_painter` / `client_side_candidate_adapter`。窗口位置仍交给各自的
原生前端；绘制、布局、选中高亮和鼠标候选区域来自同一个 frame。

## classicui（X11/XWayland、原生 Wayland popup）

`fcitx5-tipe-painter.patch` 基于 Fcitx 5.1.21，固定提交
`1319952f284eae17a36cba9e800843ca61a163c1`。仅当当前输入法唯一名为 `tipe`
时进入共享 painter。其他输入法继续使用上游主题与 Yoga 布局。

```sh
./integration/build-classicui-grid.sh
```

构建依赖与 Fcitx classicui 相同，另需支持 C++20 的编译器。脚本会初始化
固定版本的 Yoga 子模块，并输出 `libclassicui.so` 与 `libwaylandim.so` 路径。可用
`TIPE_FCITX_SOURCE_DIR`、`TIPE_FCITX_BUILD_DIR`、`TIPE_BUILD_JOBS`
设置独立源码目录、构建目录和并行度；脚本不会 reset 已有源码。

构建不会安装或重启输入法。已有用户目录安装流程可显式指定产物：

```sh
TIPE_CLASSICUI_SO=/实际构建目录/bin/libclassicui.so ./scripts/install-user.sh build
```

安装器会同时发现相邻的 `libwaylandim.so`，也可通过 `TIPE_WAYLANDIM_SO`
显式指定。两者都只安装到用户 addon 目录，纳入可恢复的安装清单。

`fcitx5-wayland-activation.patch` 修复原生 text-input-v3 的状态确认时序：
客户端可能等待 `done` 才继续发送用途与光标矩形，前端应在状态事务后立即
确认，同时保留当前 preedit，不能用空事务清掉正在输入的拼音。
每次激活都等待新的字段用途；未收到时临时使用原文输入并禁止学习，收到后
恢复该字段的正常能力。这样不会把上一个普通文本框的能力误用于密码框，
也不需要猜测应用名称、重放首个字符或启动第二个输入法连接。

core 继续用 `TIPE_CLASSIC_GRID=1` 发送展开模式的 Vertical layout hint。
绘制分支本身由当前输入法唯一名选择，不会让别的输入法变成 TiPE 样式。
XCB 沿用 Fcitx 检测到的显示器 DPI；Wayland 沿用原 popup 的 buffer scale。
鼠标 hover/click 使用相应缩放后的同一单元格，原有候选选择和滚轮翻页保留。

不支持 EWMH popup 归属的 Xwayland 桥接下，TiPE 的 XIM 候选使用客户窗口内的
子窗口回退。收到原生 Wayland 输入上下文的焦点事件时，会立即隐藏该回退窗口；
这不依赖 X11 是否同时发出 FocusOut，避免 Wine 与原生应用之间切窗后留下旧候选。
组合仍由各自输入上下文持有。该路径不读取 niri 坐标，也不轮询窗口位置。

旧 `fcitx5-tipe-grid.patch` 是早期专用 Yoga 网格实现，仅保留作历史参考；
新构建脚本不再使用它，也不应与共享 painter 补丁叠加。

## GTK3 / GTK4

见 [GTK-PAINTER.zh-CN.md](GTK-PAINTER.zh-CN.md)。GTK 的共享 `inputwindow`
分支保留 application-owned popup 和 `ClientSideInputPanel` 协议，不使用
强制清除 capability 的方式切换绘制路径。

## XIM 父窗口移动后的重绘

`fcitx5-xim-parent-damage.patch`接在共享绘制器补丁之后，由
`build-classicui-grid.sh`自动应用。缺少EWMH弹窗归属能力的Xwayland路径将候选
嵌入实际XIM客户窗口；跨输出移动时，该客户可能更换backing pixmap而不向子窗
发送Expose。补丁订阅父窗口结构通知，在ConfigureNotify后重绘可见候选，不轮询、
不猜compositor坐标、不改变组合状态。失败截图/drawable对照和三种混合缩放
证据位于`docs/evidence/2026-10-22/xim-mixed-scale-*`。
