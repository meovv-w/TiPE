# 安装与恢复

以下命令均从仓库根目录运行。先完成 [首页的核心构建](../README.md#开始使用)，再按应用需要安装对应前端。本文展示默认路径；上游开发依赖、固定版本与覆盖变量见 [集成说明](../integration/README.md)。

## 构建完整候选窗

这些脚本会获取固定版本的上游源码；构建需要联网。额外依赖包括 ECM、xkbcommon、Wayland、XCB 相关开发文件，GTK 前端需要 GTK3/GTK4 开发包，Qt 前端需要与运行时匹配的 Qt6/QtWayland 私有开发文件。具体包名随发行版变化。

```bash
# Fcitx daemon 内的原生 Wayland / X11 候选窗
./integration/build-classicui-grid.sh

# 按使用场景构建 GTK、Qt6 前端
./integration/build-gtk-painter.sh
./integration/build-qt6-tipe.sh
```

默认路径：

- Fcitx：`${XDG_CACHE_HOME:-$HOME/.cache}/tipe/fcitx5-classicui/build`
- GTK：`${XDG_CACHE_HOME:-$HOME/.cache}/tipe/fcitx5-gtk/build`
- Qt6：`build/integration/qt6-prefix`

这些入口不会替换系统库或自动重启应用。Qt 脚本会把产物放入构建目录的 prefix。

## 用户目录安装

```bash
# 查看计划与 staging 内容
./scripts/install-preview.sh build

# 安装核心、classicui，以及相邻的 waylandim
TIPE_CLASSICUI_SO="${XDG_CACHE_HOME:-$HOME/.cache}/tipe/fcitx5-classicui/build/bin/libclassicui.so" \
  ./scripts/install-user.sh build

# 先预览，再安装实际 GTK / Qt 产物
./scripts/install-client-ui.sh --dry-run \
  --gtk-build "${XDG_CACHE_HOME:-$HOME/.cache}/tipe/fcitx5-gtk/build" \
  --qt-prefix "$PWD/build/integration/qt6-prefix"
./scripts/install-client-ui.sh \
  --gtk-build "${XDG_CACHE_HOME:-$HOME/.cache}/tipe/fcitx5-gtk/build" \
  --qt-prefix "$PWD/build/integration/qt6-prefix"

# 备份 profile，重启当前用户 Fcitx 并切换到 TiPE
./scripts/activate-user.sh
```

只使用 GTK 或 Qt 时，可只传对应参数。已有受管理的前端安装，更新时加 `--force`；它仍会保护用户后来修改的文件。

`install-client-ui.sh` 会生成用户环境配置与 `~/.local/bin/tipe-run`。已经打开的应用不会立即加载新模块，可在方便时重新打开，或显式启动一个新应用：

```bash
~/.local/bin/tipe-run 你的应用命令
```

安装器保存现有 `fcitx5.service` 的专用 drop-in，让服务下次登录继续加载 TiPE。无此用户服务的桌面应使用 `scripts/run-user-fcitx.sh` 启动，并在自己的启动管理方式中引用它；安装器不会猜测或修改 niri 启动配置。

确认是否选中：`fcitx5-remote -n` 应返回 `tipe`。打开控制中心：`~/.local/bin/tipe-control`。

## 恢复与卸载

```bash
./scripts/uninstall-user.sh --dry-run
./scripts/uninstall-user.sh
```

卸载在线移除 TiPE 输入法条目，保留其他输入法。文件按安装清单恢复或删除，用户后续修改和原备份会保留并报告冲突。学习数据、导出文件和 profile 备份默认保留；删除学习数据请使用控制中心“清空”。

不要在运行中的 Fcitx 外部直接编辑 profile。`enable-user.sh` / `disable-user.sh` 是停 daemon 后才使用的底层命令，日常使用启用脚本即可。

Wine 的光标跟随配置见 [Wine 说明](WINE.zh-CN.md)；协议能力和隐私限制见 [用户说明](USER_GUIDE.zh-CN.md)。
