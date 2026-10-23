# 已安装 niri 的隔离输入路径

## 范围与隔离

系统为niri 26.04。当前实体桌面只有eDP-1，不能把其他compositor的双输出
结果当成niri实体双屏通过。本轮新增`scripts/check-niri-isolated.py`：私有
D-Bus、HOME/XDG、无屏幕Sway及其虚拟seat内运行未修改的系统niri程序；
所有客户端和Fcitx均属于该niri的独立Wayland socket。没有使用`--session`、
导入全局环境、改变当前桌面输出或写系统输入设备。

测试从1×开始一段活动组合，再改变niri输出缩放，直接从内层niri的screencopy
读取画面。客户端协议的caret/anchor矩形证明定位，截图证明完整可见和展开
宽度保持，控件/DOM/终端stdin读回固定中文证明提交。每次按键、指针动作前
核对外层niri窗口和内层客户端两级焦点、窗口ID及进程身份。

这是单输出动态缩放验证，不是同时双输出或跨屏迁移。GTK/Qt、foot原生协议、
Wine/XIM的隔离双屏证据仍分别来自Mutter和Sway。

## 为什么不能直接启用niri的headless

安装程序的帮助没有无屏幕后端启动选项。进一步检查上游v26.04源码：

- `src/main.rs`的`State::new`明确传入`headless=false`。
- `src/niri.rs`仅在该参数为true时选择`Backend::Headless`，有父显示连接时
  选择Winit，否则选择TTY；没有用于切换headless的环境变量。
- `src/backend/headless.rs`供内部测试创建输出，常规render只发送帧回调，
  dmabuf导入未实现。不能仅凭二进制包含headless字符串就启动并宣称通过。

审计源码从`https://github.com/YaLTeR/niri/tree/v26.04`取得，SHA256：

| 文件 | SHA256 |
| --- | --- |
| src/main.rs | d1aab6636ab466d185a586036735b6d3a2c5e19c400d6e6414438d81ec7a4ec7 |
| src/niri.rs | a619642c264727a6359eeef186ae45e6d41a566e71d64fdddbe85bca389ce1c2 |
| src/backend/headless.rs | f28628846f9f2f02d94a79cea543bcaa9baa7310e187817ebe814a3d6c1dc8e3 |

没有编译、安装或替换用户compositor。

## 键盘路径校正

首次尝试把wtype输入送到外层，发现其动态分配的临时键码经过Winit后被解释为
不同字符（`nihao`变成数字）。这是测试夹具错误，未计为TiPE回归失败或通过。
随后新增`tests/desktop_keyboard.c`，用xkbcommon创建标准evdev/pc105/us映射，
向外层私有虚拟键盘发送固定Linux键码。这样Winit收到真实键盘事件，再经过
niri的普通键盘处理及Fcitx输入法抓键，不在内层用虚拟键盘绕过抓键。

新增XML保留原协议许可。软件渲染测试以较低优先级运行；后续启动限制Mesa
软件渲染线程，避免测试争用交互桌面的计算资源。它不是产品性能基准。

最终逐项结果、实际模块映射和日志由`niri-isolated-results.json`、
`niri-isolated-runtime.json`、`niri-isolated-final.log`记录；这些文件仅在
完整运行结束并检查后归档。
