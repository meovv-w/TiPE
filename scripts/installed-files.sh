#!/usr/bin/env bash

tipe_build_source_fingerprint() {
    local root="$1"
    (
        cd -- "$root" || exit 1
        find CMakeLists.txt src tests tools scripts addon data third_party \
            -type f ! -path '*/__pycache__/*' ! -name '*.pyc' -print0 |
            LC_ALL=C sort -z |
            xargs -0 -r sha256sum --zero |
            sha256sum |
            awk '{print $1}'
    )
}

tipe_user_home() {
    if [[ -z "${HOME:-}" ]]; then
        echo "HOME is not set; TiPE user install paths are unavailable" >&2
        return 1
    fi
    printf '%s\n' "$HOME"
}

tipe_helper_install_pairs() {
    local root="$1"
    local user_home
    user_home=$(tipe_user_home) || return 1
    printf '%s\t%s\n' \
        "$root/scripts/model-protocol-example.sh" "$user_home/.local/bin/tipe-model-protocol-example" \
        "$root/scripts/model-adapter.sh" "$user_home/.local/bin/tipe-model-adapter" \
        "$root/scripts/model-dump.sh" "$user_home/.local/bin/tipe-model-dump" \
        "$root/scripts/model-explain.sh" "$user_home/.local/bin/tipe-model-explain" \
        "$root/scripts/learning-panel.sh" "$user_home/.local/bin/tipe-learning-panel" \
        "$root/scripts/supervision-window.sh" "$user_home/.local/bin/tipe-supervision-window" \
        "$root/scripts/analyze-window.sh" "$user_home/.local/bin/tipe-analyze-window" \
        "$root/scripts/model-replay.sh" "$user_home/.local/bin/tipe-model-replay" \
        "$root/scripts/model-current.sh" "$user_home/.local/bin/tipe-model-current" \
        "$root/scripts/model-config.sh" "$user_home/.local/bin/tipe-model-config" \
        "$root/scripts/model-self-test.sh" "$user_home/.local/bin/tipe-model-self-test" \
        "$root/scripts/model-wrapper-new.sh" "$user_home/.local/bin/tipe-model-wrapper-new" \
        "$root/scripts/model-wrapper-check.sh" "$user_home/.local/bin/tipe-model-wrapper-check" \
        "$root/scripts/training-export.py" "$user_home/.local/bin/tipe-training-export" \
        "$root/scripts/personal-model.py" "$user_home/.local/bin/tipe-personal-model" \
        "$root/scripts/personal-model-train.sh" "$user_home/.local/bin/tipe-personal-model-train" \
        "$root/scripts/check-user-dictionary.sh" "$user_home/.local/bin/tipe-check-user-dictionary" \
        "$root/scripts/check-preferences.sh" "$user_home/.local/bin/tipe-check-preferences" \
        "$root/scripts/doctor.sh" "$user_home/.local/bin/tipe-doctor" \
        "$root/scripts/restart-fcitx5.sh" "$user_home/.local/bin/tipe-restart-fcitx5" \
        "$root/scripts/toggle.sh" "$user_home/.local/bin/tipe-toggle"
}

tipe_installed_files() {
    local user_home
    user_home=$(tipe_user_home) || return 1
    printf '%s\n' \
        "$user_home/.local/bin/tipe-candidate-window" \
        "$user_home/.local/bin/tipe-learning-panel-window" \
        "$user_home/.local/bin/tipe-state-probe" \
        "$user_home/.local/lib64/fcitx5/libtipe.so" \
        "$user_home/.local/lib64/fcitx5/libtipeui.so" \
        "$user_home/.local/lib/fcitx5/libtipe.so" \
        "$user_home/.local/lib/fcitx5/libtipeui.so" \
        "$user_home/.local/share/fcitx5/addon/libtipe.so" \
        "$user_home/.local/share/fcitx5/addon/libtipeui.so" \
        "$user_home/.local/share/fcitx5/addon/tipe.conf" \
        "$user_home/.local/share/fcitx5/addon/tipeui.conf" \
        "$user_home/.local/share/fcitx5/inputmethod/tipe.conf" \
        "$user_home/.local/share/applications/tipe-supervision.desktop" \
        "$user_home/.local/share/icons/hicolor/scalable/apps/tipe.svg" \
        "$user_home/.local/share/icons/hicolor/scalable/apps/dev.tipe.LearningPanel.svg" \
        "$user_home/.local/share/tipe/support/wechat.png" \
        "$user_home/.local/share/tipe/support/alipay.png"
    local wine_bridge="$user_home/.local/libexec/tipe/tipe-wine-caret-bridge.exe"
    if [[ -f "${TIPE_SOURCE_ROOT:-.}/build/tipe-wine-caret-bridge.exe" || -f "$wine_bridge" ]]; then
        printf '%s\n' "$wine_bridge"
    fi
    local _source destination
    while IFS=$'\t' read -r _source destination; do
        printf '%s\n' "$destination"
    done < <(tipe_helper_install_pairs "${TIPE_SOURCE_ROOT:-.}")
}
