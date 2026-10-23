# 原生 Wayland 候选适配路线

Fcitx 5.1.21 有两条候选窗口路径，必须先区分它们：

* `GTK_IM_MODULE=fcitx` 和 `QT_IM_MODULE=fcitx` 使用 D-Bus frontend。GTK/Qt
  frontend 声明 `ClientSideInputPanel`，Fcitx 通过
  `UpdateClientSideUI(a(ss), candidateIndex, layoutHint, hasPrev, hasNext)`
  发送可见候选页；popup 由客户端创建。GTK4 的
  `Gtk4InputWindow` 使用 `GtkPopover`，Qt6 的
  `FcitxCandidateWindow` 使用 Qt Wayland 私有 xdg-shell wrapper。两者都能
  让 compositor 根据父 surface 和 text rectangle 定位，不需要 TiPE 连接
  第二个 input-method，也不需要猜 niri 全局坐标。
* `GTK_IM_MODULE=wayland` 或 `QT_IM_MODULE=wayland` 使用 Fcitx
  `waylandim` frontend。此时 Fcitx classic UI 的私有
  `WaylandInputWindow` 调用 `ZwpInputMethodV2::getInputPopupSurface()`；
  popup surface 属于现有 input-method context。该 C++ wrapper 没有安装到
  `fcitx5-devel` 的公共 ABI，TiPE addon 不应通过 reinterpret_cast 或复制
  类布局调用它。

D-Bus frontend 的 display 路由不是空值 bug：GTK/Qt 的 gclient 明确发送
`display="wayland:"`，Wayland module 默认连接也建立 `FocusGroup` 名
`wayland:`。真正容易丢候选的是以下条件之一：

1. GTK/Qt 的 fcitx platform input context 没有建立（`GTK_IM_MODULE`/
   `QT_IM_MODULE` 被 ibus 或 toolkit 默认值覆盖）；
2. D-Bus capability 没有保留 `ClientSideInputPanel`，于是 Fcitx 不发
   `UpdateClientSideUI`，而 UI addon 又没有合法的 client parent surface；
3. client 收到候选后使用 `candidateIndex` 作为全局索引，或在过滤
   placeholder 后没有同步索引，导致高亮和点击偏移。

`include/tipe/client_side_candidate_adapter.hpp` 与
`src/client_side_candidate_adapter.cpp` 是一个不拥有 Wayland 连接的协议
适配层，已接入下列固定版本的客户端构建。它消费 D-Bus 的可见候选页，强制 page-local cursor 落在有效 cell，
把 Vertical hint 映射到 TiPE expanded grid，并把 scale 交给共享
`candidate_painter`。GTK/Qt frontend 的维护性集成点分别是：

* GTK4：在共享的 `gtk3/inputwindow.cpp::InputWindow::updateUI` 把 `a(ss)`
  转成 `client_side::Update`，用 `painter::paint` 替换 cell 绘制和
  size hint；继续使用 `gtk4/gtk4inputwindow.cpp` 已有的 `GtkPopover`
  parent 和 `gtk_popover_set_pointing_to`。
* Qt6：在 `qt6/platforminputcontext/fcitxcandidatewindow.cpp` 的
  `updateClientSideUI` 中使用相同 adapter；保留 Qt 6.6–6.9 的
  `xdg_popup_reposition` 兼容代码，以及 Qt 6.10 之后消费
  `_q_waylandPopupAnchorRect` 的 Qt Wayland backend 路径。

这两个 frontend patch 以 Fcitx5-gtk/Fcitx5-qt 的明确版本为构建依赖，
通过独立 user prefix 或应用环境启用；候选绘制不靠替换 daemon 的
`waylandim` addon，也不启动第二个 `zwp_input_method_manager_v2` client。为覆盖
`GTK_IM_MODULE=wayland` 的 daemon-side popup，已对 Fcitx classic UI 做同一
版本的 source build，在 `waylandinputwindow.cpp` 中直接调用私有 wrapper；这
是可维护的 fork/overlay，不能伪装成普通 UI addon 的 ABI 适配。

适配层单测 `tipe-client-side-adapter-tests` 覆盖空页、Vertical 展开、NaN
scale 和延迟 signal 的 cursor clamp。它验证候选数据变换；用户前缀中的真实模块由 GTK4/Qt6 桌面夹具另行验证。
2026-10-21 两个位置均完成真实物理键、Wayland 指针、候选截图和控件文本读回；
产品定位只用原生父 surface 和光标矩形。测试脚本移动的仅是自己创建的窗口。

## XIM / Xwayland 桥接回退

Wine默认callbacks样式可能只提供初始spot。当前另提供可恢复的按应用
`overthespot`配置，使用Wine标准XIM Position能力，实际Win32光标横移、换行
和19项编辑检查已通过；详见[Wine配置说明](WINE.zh-CN.md)。以下的初始坐标
限制指默认callbacks，不代表所有Wine输入样式。

xwayland-satellite 0.8.1 的 override-redirect popup 归属可能取最后被鼠标
进入的窗口，忽略 WM_TRANSIENT_FOR。仅用键盘切入 Wine 时，X11 drawable
虽然已映射，像素仍可能没有到达当前桌面。TiPE classicui 在 Xwayland
不声明 EWMH popup 类型支持时，将候选窗作为实际 X11 焦点顶层窗口的子窗口。
本回退只用于 TiPE 的 XIM 上下文，不影响其他输入法。

位置通过 XIM 的 root-relative 矩形和 xcb_translate_coordinates 换算，
限制在客户 drawable 内，底部空间不足时向上摆放；不查询 niri 坐标。
父窗口消失后延迟重建 drawable，避免在正在执行的事件回调里替换自己。
每个 daemon 会话只记录一次回退诊断，避免连续输入刷日志。

Wine 11 默认 XIM callbacks 模式可能始终提供初始 (0,0) spot；候选窗
因此只能锚定客户窗口顶部，而不能精确跟随 Win32 caret。TiPE 不将这个
矩形伪装成准确光标。子窗口也会受客户边界裁剪。这是明确的协议能力回退，
不能等同于完整光标跟随验收。设置 TIPE_XIM_EMBED_POPUP=0 可禁用，
设为 1 可强制启用；该设置需在启动 daemon 时传入。

Xwayland 若使用物理像素根窗口但未提供 Xft.dpi，候选窗可能只有原生
Wayland 窗口的一半字号。TiPE 只在已有 Wayland 连接恰好报告一个输出、
其物理模式与 X11 root 尺寸完全一致时采用该输出的 scale。若 X11 root
已经是逻辑尺寸，或多输出映射不明确，则保留 X11 自己的 DPI；不猜倍率。
当前 3120×2080 / 2× 输出下，Wine 候选 drawable 从598×158修正为1196×316。

## 原生 Wayland 的首次按键与敏感字段

Chromium 150 的 text-input-v3 会在确认前暂存用途和光标信息。仅在按键时
更新 preedit，会让密码标志晚于首个按键；Fcitx 的公共 `updatePreedit()`
还会抑制重复的空 preedit，不能用它代替前端的协议确认。

用户前缀中的 `fcitx5-wayland-activation.patch` 基于同一固定的 5.1.21
源码，在每个已激活状态事务后确认，并携带当前已有的 preedit 保持组合。
新字段没有提供用途时，先以 Sensitive + Disable 走原文路径，不观察键盘、
不查询个人学习、不显示中文候选；收到本字段的用途后恢复正常输入。
这是协议能力回退，不使用浏览器名称分支。不能提供用途的客户端会保持
原文回退，诊断日志会说明原因；优先使用能提供用途的 GTK/Qt 桥接。

XIM 本身不能传递敏感字段标志。真实 Wine ES_PASSWORD 仍将按键送给
输入法，因此 TiPE 默认对全部 XIM 输入关闭观察、个人学习和云端工作，
保留正常中文输入。不能把“没有密码标志”当作允许记录的证据。

2026-10-22 的真实夹具同时检查字符完整、观察计数和模型内容。浏览器还
检查密码框返回普通文本框后能继续中文输入、恢复学习，以及空密码框往返
不会改变选中的文本。完整证据与当前限制见验收表。

## 分段组合缩短后的删除恢复

真实 Chromium 分段夹具发现：`nihao` 选“你”，删空剩余 `hao` 后再按退格，
前端已发送正确的三字节删除，但浏览器的周围文本缓存仍带有先前组合长度，
拒绝删除并留下“你ni”。只补一个空 preedit 通知不能解决这个问题。

固定版本的 Wayland overlay 现在将组合文本变化与
`delete_surrounding_text(0, 0)` 放入同一个协议事务。
根据 text-input-v3 的顺序，先移除旧 preedit，再删除指定的周围文本，再插入
提交与新 preedit。零长度请求不删除文档字符，使客户端同步更新组合范围。
只有已知周围文本且实际组合文本改变时才发送；状态确认重发相同组合时不发。
提交文字会改变文档坐标，因此先等待新的周围文本快照，再允许此同步请求，
避免针对旧组合范围处理新文档。
组合结束只显式清空一次，缓存受 Wayland 字符串上限约束，不加入定时器或轮询。
该规则用于协议前端，不按应用名分支，也不修改浏览器。

核心展开候选另有一项独立修复：LibIME `candidatesToCursor()` 的结果可能跨过
请求位置，消耗长度必须取候选句子图的真实终点，不能直接使用请求光标位置。
这避免完整“你好”被错误标成只消耗三或四个字母，以及重复候选和错误后缀。
核心回归对所有“你好”/“你”的边界和提交后缀逐一断言。
