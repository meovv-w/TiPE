# GTK、Qt 与终端协议回归

`tests/test_protocols.cpp` 在独立的 fcitx5 `testfrontend` 实例中运行三组协议画像：

| 画像 | 模拟的应用标识 | 关键能力 | 验证内容 |
| --- | --- | --- | --- |
| GTK | `gtk3-regression` | `Preedit`、`FormattedPreedit`、`SurroundingText`、`KeyEventOrderFix` | `nihao` 提交“你好”、英文原文提交、text rectangle |
| Qt | `qt6-regression` | `Preedit`、`SurroundingText`、`CommitStringWithCursor`、`KeyEventOrderFix` | 同上，并覆盖带光标提交能力 |
| 终端 | `foot-regression` | `Terminal`、`Preedit`、`KeyEventOrderFix` | 同上，并覆盖终端能力标志 |

运行：

```bash
./scripts/run-protocol-regression.sh build
```

脚本会重新构建协议测试目标（并在目标存在时复用 `tipe-fcitx-tests`），记录 `fcitx5`、GTK、Qt、foot 和当前显示协议版本，并把可复现输出保存为 `build/protocol-regression.log`。测试前端会发送 key down 和 key up，设置光标矩形，再检查真实 `commitString`；它不启动浏览器、GTK/Qt 窗口或终端，也不对候选主题做视觉断言。这样可在无焦点窗口、无 compositor 截图的 CI 环境中验证输入协议核心行为，同时避免把“插件文件存在”误认为“输入法已加载”。

真实应用仍需一次集中观察。记录应用版本、显示协议（Wayland 或 XWayland）、输入法切换方式、候选窗锚点和提交结果。推荐顺序是 GTK 文本编辑器、Qt 文本编辑器、foot；浏览器、Discord 和 Wine 若使用不同前端，再单独记录。若出现问题，同时保存 `journalctl --user -u fcitx5` 中 TiPE 的加载和协议错误行。
