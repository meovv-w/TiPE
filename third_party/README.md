# Third-Party Sources

## fcitx-wayland 5.1.19 subset

`fcitx-wayland-5.1.19/` contains the small generated Wayland wrapper subset
needed by TiPE's input-method-v2 popup implementation. It was taken from the
fcitx5 5.1.19 source release:

- Upstream: https://github.com/fcitx/fcitx5
- Version: 5.1.19
- Wrapper license: LGPL-2.1-or-later; see
  `fcitx-wayland-5.1.19/LGPL-2.1-or-later.txt`
- `input-method-unstable-v2.xml`: its MIT-style copyright and permission text
  is embedded directly in that file.

Only the files listed under this directory are vendored. The rest of fcitx5
is consumed through the installed development libraries and headers.
