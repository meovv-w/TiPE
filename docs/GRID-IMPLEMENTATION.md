# 候选多列网格的接入证据与最短实现路径

本文记录在本机 Fcitx 5.1.21 头文件和上游源码中查到的接口边界。它用于后续接入 `candidate_grid`，不把标准候选列表的横向/纵向提示误称为真正的多列网格。

## 已确认的公开 API

本机安装的头文件位于 `/usr/include/Fcitx5`，版本由 `pkg-config --modversion Fcitx5Core Fcitx5Utils` 报告为 **5.1.21**。

* `Core/fcitx/candidatelist.h` 的 `CandidateLayoutHint` 只有 `NotSet`、`Vertical`、`Horizontal`。`CommonCandidateList::setLayoutHint()` 只能选择单轴布局；`PageableCandidateList` 提供翻页，`CursorMovableCandidateList` 提供前后移动，不能表达行列矩阵或每个候选的矩形。
* `Core/fcitx/inputpanel.h` 的 `InputPanel::setCustomInputPanelCallback(CustomInputPanelCallback)`（5.0.24 起）可以让 UI 更新时调用输入法注册的回调。回调参数只有 `InputContext *`，输入法可以读取 `inputPanel().candidateList()`，但 Fcitx 不提供绘制画布。
* `Core/fcitx/inputcontext.h` 提供 `cursorRect()`、`scaleFactor()`、`updateUserInterface()`、`commitString()` 和 `deleteSurroundingText()`。这些是定位和选择提交所需的公开协议数据。
* `Module/fcitx-module/wayland/wayland_public.h` 只公开 Wayland 连接生命周期回调、`wl_display *`、`FocusGroup *` 和打开连接函数。没有公开的“创建输入法候选 popup”或“设置 text rectangle”的高层函数。

上游 5.1.21 的 `src/lib/fcitx/userinterfacemanager.cpp`（本机源码审计副本中的 135–151 行）给出了回调优先级：物理键盘模式下，若 `InputPanel::customInputPanelCallback()` 存在，回调会在 classic UI `UserInterface::update()` 之前被调用；因此输入法可以接管一次 InputPanel 更新。但回调仍须自行拥有绘制窗口和输入事件处理。若要成为常驻 UI addon，则使用 `UserInterface::update()`，并在 addon 配置中声明 `Category=UI`、`UIType=PhyscialKeyboard` 和足够的 `UIPriority`；`UserInterfaceManager::load()` 按 `UIPriority` 排序，故会替换同模式的 classicui。

## 为什么标准 UI 不能满足网格

上游 Fcitx `src/ui/classic/inputwindow.cpp` 的公开实现使用 Yoga 布局：候选容器根据 `CandidateLayoutHint` 设置 `YGFlexDirectionRow` 或 `YGFlexDirectionColumn`，每个候选内部仍是“编号 + 文本 + comment”的单行节点。候选命中区域在内部 `candidateRegions_` 中生成，类定义在 `src/ui/classic/inputwindow.h`，不是公共头文件。`Vertical` 因而是单列列表，`Horizontal` 是单行列表，两者都无法实现动态多列、多行的 macOS 式网格。

同一份源码显示，Wayland 经典 UI 的窗口实现是私有的 `fcitx::classicui::WaylandInputWindow`。它自行绑定 `zwp_input_panel_v1` 或 wayland-im v2 的 `getInputPopupSurface()`，使用 Cairo 绘制，并通过 `InputContext::cursorRect()` 间接定位。该类和 `WlShm`/`WlCompositor` 包装器均不在安装的公共 Fcitx API 中，不能从 TiPE addon 安全复用。

可复核的上游源码（与本机 5.1.21 API 对应的实现）：

* [InputPanel API](https://github.com/fcitx/fcitx5/blob/5.1.21/src/lib/fcitx/inputpanel.h)（项目安装路径以 `/usr/include/Fcitx5/Core/fcitx/inputpanel.h` 为准）
* [CandidateList API](https://github.com/fcitx/fcitx5/blob/5.1.21/src/lib/fcitx/candidatelist.h)
* [Classic input layout](https://github.com/fcitx/fcitx5/blob/5.1.21/src/ui/classic/inputwindow.cpp)，尤其是 `layoutHint_`、Yoga flex direction 和 `candidateRegions_`
* [Wayland classic window](https://github.com/fcitx/fcitx5/blob/5.1.21/src/ui/classic/waylandinputwindow.cpp)，其中 `getInputPanelSurface`、`getInputPopupSurface` 和 Cairo `paint()` 均为 UI addon 私有实现
* [InputPanel callback dispatch](https://github.com/fcitx/fcitx5/blob/5.1.21/src/lib/fcitx/userinterfacemanager.cpp#L135-L151)，确认物理键盘模式下 custom callback 位于 classic UI fallback 之前

## 最短可实际实现方案

1. 让 TiPE 输入法 addon 继续用 `CommonCandidateList` 负责键盘分页和可访问性，同时把候选数据和 `candidate_grid::Layout` 交给一个**独立 UI addon**（而不是在输入法 addon 中强行画）。`setCustomInputPanelCallback` 是输入法在单个 `InputPanel` 上注册回调的替代路径，并不提供绘制画布；它可以作为低层触发钩子，但不能替代 UI addon。
2. 独立 UI addon 实现 `fcitx::UserInterface::update(UserInterfaceComponent::InputPanel, InputContext *)`，读取 `InputPanel` 数据，测量字体后调用 `candidate_grid::layout()`，并对每个 `Cell::rect` 进行 Cairo 绘制和 hit-test。候选点击调用 `CandidateWord::select(ic)`；键盘方向键仍由输入法状态机处理。
3. Wayland 后端优先复刻 classicui 的私有窗口路径：通过 `wayland_v2` 的 wayland-im addon 获取 `getInputPopupSurface()`；旧 `wayland` 前端使用 `zwp_input_panel_v1.get_input_panel_surface()` + `set_overlay_panel()`。这需要复制/封装所需的 generated protocol 和 shm/Cairo 窗口代码，因为 `wayland_public.h` 本身只给 `wl_display *`，没有 popup helper。
4. X11/XWayland fallback 使用独立顶层 override-redirect 窗口，锚点取 `ic->cursorRect()`，并把 `ic->scaleFactor()` 应用于像素尺寸；不能读取 niri 坐标或轮询 compositor。没有可用 popup 能力时，应退回标准 InputPanel 并写一条诊断状态。

这个方案的关键是**真正绘制每个 Cell 的矩形**，而不是把 `setLayoutHint(Vertical)` 改名为网格。`candidate_grid` 纯模块已经提供紧凑模式、动态行列、候选宽度、分页标签、方向导航、previous/next page 和 hit-test；UI addon 只负责字体测量、绘制、Wayland surface 生命周期和候选选择。

## 依赖和限制

当前系统可用：Fcitx Core/Utils 5.1.21、Wayland client headers/libraries、Cairo/Pango（classicui 依赖）。当前系统未提供公开的 Fcitx 自绘 UI SDK，也未提供可由输入法 addon 直接调用的 `InputPopup` C++ 类。当前 `tipe-ui` 已完成公开 API 诊断渲染和 X11/XWayland override-redirect fallback；Wayland v2 popup 仍需由 Fcitx 内部 wayland-im 对象提供，无法仅凭 `wayland_public.h` 安全创建。

上游 5.1.21 源码另有 `src/frontend/waylandim/waylandim_public.h`，其中
声明了 `WaylandIMModule::getInputMethodV2(InputContext *)`。这只是 addon
模块的构建期导出：本机 Fedora `fcitx5-devel` 没有安装该头文件；即使随
TiPE 复制这份声明，它返回的 `fcitx::wayland::ZwpInputMethodV2` 也只是
前置声明。`getInputPopupSurface()` 定义在未安装的
`src/lib/fcitx-wayland/input-method-v2/zwp_input_method_v2.h` wrapper 中，
其生成协议对象和私有 ABI 不能靠 reinterpret_cast 或手工声明复用。这样
做会把 TiPE 绑定到 Fcitx 内部类布局和发行版构建细节，升级后可能崩溃，
所以当前实现保持 `wayland_public_popup_factory:false` 并回退标准 InputPanel。

`tipe-ui` 现在会按公开 `WaylandModule` 的连接生命周期回调记录
`wayland_module_loaded`、`wayland_im_module_loaded`、`wayland_display_env`、
`wayland_connection_active` 和连接名；同时明确写入
`wayland_popup_supported:false` 与 `wayland_fallback:"standard-input-panel"`。
这能区分“客户端设置了 `WAYLAND_DISPLAY`”和“Fcitx 确实有 Wayland
连接”，也避免把环境变量或 XWayland 窗口误报为原生 popup。对应的
`tipe-ui-wayland-capability-diagnostic` CTest 只验证诊断契约，不冒充真实
Wayland compositor 的视觉或鼠标验收。

每一帧还记录 `InputContext::cursorRect()` 的原始
`cursor_rect_x/y/width/height`。这些值是前端提供给 Fcitx 的文本矩形（坐标
系取决于前端），可用于真实桌面回归核对候选窗是否跟随插入点；TiPE 不把它们
擅自转换成 niri/compositor 的全局坐标。Wayland input-method-v2 的真正
锚点另由 `zwp_input_popup_surface_v2.text_input_rectangle` 事件传给 popup
对象；Fcitx classicui 的私有 `WaylandInputWindow` 通过该对象由 compositor
放置，普通 UI addon 无法订阅这个事件。因此 event-log 的 cursorRect 字段
不能冒充 Wayland 原生 popup 的锚点证据。

在连接创建回调中，`tipe-ui` 还对同一个公开 `wl_display` 做一次只读
`wl_registry` 探测，记录 `wayland_global_probe`（`complete`、`error` 或
`not-run`）以及 `wayland_input_method_v1`、`wayland_input_panel_v1`、
`wayland_input_method_v2`、`wayland_input_method_manager_v2`、
`wayland_popup_manager_v2` 和 `wayland_popup_surface_v2`。每个 global 的
协议版本也会记录在对应的 `*_version` 字段，便于把桌面 trace 与 Fcitx
选用的协议版本对齐。探测不绑定任何 global，也不创建 surface；因此即使
compositor 宣布了 popup global，`wayland_popup_supported` 仍保持 `false`，
因为 addon 没有 Fcitx 私有 `wayland-im` 对象的工厂和协议 wrapper，继续
使用标准 InputPanel。帧同时写入 `wayland_public_popup_factory:false` 和
`wayland_popup_reason:"public-api-no-popup-factory"`，让诊断能区分“协议
存在但 TiPE 无法安全接管”与“协议根本不存在”，并由 CTest 检查字段契约。

通常 Wayland registry 只公布 `zwp_input_method_manager_v2`；popup surface
是由该 manager 创建的对象，并不会自己作为 global 出现。因此
`wayland_input_method_manager_v2:true` 是“协议入口存在”的能力提示，
而不是 TiPE 已经拥有创建 surface 的权限；`wayland_popup_manager_v2` 同样
只是兼容旧实现的名称提示；`wayland_popup_surface_v2` 只记录服务端确实
公布了同名 global 的特殊情况，因为规范中的 popup surface 通常是
`get_input_popup_surface` 创建的子对象，不是 registry global。

为便于在真实桌面收集可复核证据，`scripts/probe-wayland-capabilities.sh`
会调用 `wayland-info`（没有该工具时尝试 `weston-info`），保存原始输出和
规范化的 `capabilities.json`。该脚本只连接 registry，不绑定 manager、创建
surface 或触碰用户配置；没有 socket 时返回 `status:skipped`。因此它能证明
compositor 是否公布 `zwp_input_method_manager_v2`，但 `native_popup_created`
始终为 `false`，不会把能力探测误报成候选窗口已显示。

2026-09-08 的本机复核还确认：`libclassicui.so` 内部虽然包含
`get_input_popup_surface`、`zwp_input_popup_surface_v2` 和
`set_overlay_panel` 符号，但系统没有对应的公开 C++ 头文件或可链接的
Fcitx ABI；TiPE 不依赖这些私有符号，也不读取 compositor/niri 坐标。

协议依赖应按 Fcitx 上游 `src/lib/fcitx-wayland/input-method-v2/` 的生成 wrapper 方式随 addon 构建（`zwp_input_method_v2.h`、`zwp_input_popup_surface_v2.h` 以及对应 `wayland-input-method-unstable-v2-client-protocol.h`）；这些文件不在 `/usr/include/Fcitx5`，不能把 classicui 的私有头文件路径当作系统 ABI。`wayland_public.h` 可用于发现连接，但不够单独创建 popup。诊断还记录 `wayland_im_module_loaded`：即使发行版加载了 Fcitx 的 `waylandim` 前端，TiPE 仍只拥有公开连接回调，不能把该布尔值当成 popup 工厂授权。
