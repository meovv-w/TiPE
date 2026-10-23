# Fcitx classicui TiPE 网格补丁

`fcitx5-tipe-grid.patch` 针对 Fcitx 5.1.22（源码提交 `2a5694d`，运行时 5.1.21） 的 `src/ui/classic/inputwindow.cpp`。它给 classicui 增加真实 Yoga flex-wrap 候选网格：每格默认 88×32 px（按候选内容在 72–128 px 内自适应），三列，后续候选自动换行；原有 `candidateRegions_` 仍从 Yoga 节点读取，因此鼠标命中、悬停和分页继续有效。

## 构建

```sh
git clone https://github.com/fcitx/fcitx5 /tmp/fcitx5
git -C /tmp/fcitx5 checkout 2a5694d
patch -d /tmp/fcitx5 -p1 < /home/lin/Documents/ChatGPT/tipe/integration/fcitx5-tipe-grid.patch
cmake -S /tmp/fcitx5 -B /tmp/fcitx5-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DCMAKE_SKIP_RPATH=ON -DENABLE_WAYLAND=On -DENABLE_X11=On -DENABLE_UI_CLASSIC=On
cmake --build /tmp/fcitx5-build --target classicui -j$(nproc)
```

运行 TiPE 时设置 `TIPE_CLASSIC_GRID=1`（例如在 `scripts/run-user-fcitx.sh` 的进程环境中）。未设置时 classicui 行为完全保持上游默认。
