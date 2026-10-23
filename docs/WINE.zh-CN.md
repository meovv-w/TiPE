# Wine 应用的中文输入与光标跟随

Wine 使用 X11 后端时，默认输入样式可能只给输入法初始坐标，候选窗因此停在
窗口上方。Wine 11 的标准 `overthespot` 样式会发送实际光标位置。TiPE 提供
按应用启用和恢复的工具，不需要修改 Wine 源码。

先关闭要配置的 Windows 应用。下面以已有的 `~/.wine` 和 `notepad.exe` 为例；
使用其他 Wine 环境时，把目录和程序文件名换成实际值。工具不会创建新环境。

```sh
cd /home/lin/Documents/ChatGPT/tipe
python3 scripts/configure-wine-input.py --prefix ~/.wine --app notepad.exe --dry-run
python3 scripts/configure-wine-input.py --prefix ~/.wine --app notepad.exe --apply
```

重新打开该 Windows 应用后生效。只设置指定程序的输入样式；其他程序的设置、
Wine 全局默认值、图形后端和桌面设置均不受影响。程序名仅填写 `xxx.exe`，
不填写 Windows 路径。使用启动器时，应填写实际接收键盘输入的程序文件名。

恢复原设置：

```sh
python3 scripts/configure-wine-input.py --prefix ~/.wine --app notepad.exe --restore
```

原值在修改前写入 `$XDG_STATE_HOME/tipe/wine-input`，默认目录为
`~/.local/state/tipe/wine-input`。重复启用保留最初的备份。若后来通过其他工具
修改了该设置，恢复命令会保留新值并说明原因，不会覆盖它。

Wine 的 XIM 协议不能可靠标记密码框，因此 TiPE 对整个 XIM 路径关闭观察、
个人学习和云端增强，保留中文输入。该样式不会解除这项隐私边界，也不会
为 XIM 增加周围文本能力；删除已提交文字仍交给 Windows 控件处理。

已验证 Wine 11.0 (Staging) 的标准 Win32 EDIT 控件：初始、输入 ASCII 后、
换行后三个位置与实际 Win32 光标对应；按应用配置后六项输入检查通过；
该样式的完整编辑矩阵 19/19 通过。启用与恢复的五项事务检查也通过。
另已通过隔离Sway/Xwayland双屏12项：1×与1.25×、1.5×、2×之间保留组合
双向移动，普通/长词的完整候选、逻辑宽度、实际Win32 caret与键盘/鼠标提交
均核对。修复了父窗口跨屏后替换backing pixmap、候选子窗未重绘的问题。
这些证据不等于所有 Windows 程序或实体niri多屏组合已经验证。

可重复的开发检查：

```sh
python3 scripts/check-wine-configuration.py
TIPE_HARNESS_CARET=1 TIPE_WINE_INPUT_STYLE=overthespot python3 scripts/check-wine-desktop.py
TIPE_WINE_INPUT_STYLE=overthespot python3 scripts/check-editing-desktop.py --output build/editing-wine-overthespot wine
```

这些命令只操作仓库 `build` 内的专用 Wine 环境；第一条要求先运行过
`check-wine-desktop.py`，以便专用环境已经存在。桌面输入检查须顺序执行。
