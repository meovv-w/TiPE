#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

usage() {
    cat <<'EOF'
用法：tipe-supervision-window [REQUEST_TSV]

打开 TiPE 窗口。“学习”页显示已经记录和学会了什么，“模型”页选择 TiP、
本地大模型或云端大模型，“支持”页显示支持信息。
点击分析或按 F9 时才会运行一次配置的模型；打开或刷新窗口不调用模型。
模型返回的内容经本地校验后才会写入 TiPE 的学习数据。

不传 REQUEST_TSV 时，默认读取当前 TiPE 实时监督快照。
如果当前还没有活动拼音，窗口会先等待，也可以暂时显示上一次捕获的输入快照。
这个工具不会重启 fcitx5，不会切换输入法，也不会修改全局输入法配置。
EOF
}

helper_path() {
    local helper_name="$1"
    if [[ -x "$script_dir/$helper_name" ]]; then
        printf '%s\n' "$script_dir/$helper_name"
    elif [[ -x "$script_dir/${helper_name#tipe-}.sh" ]]; then
        printf '%s\n' "$script_dir/${helper_name#tipe-}.sh"
    elif [[ -x "$script_dir/${helper_name#tipe-}.py" ]]; then
        printf '%s\n' "$script_dir/${helper_name#tipe-}.py"
    elif [[ -x "$script_dir/../build/$helper_name" ]]; then
        printf '%s\n' "$script_dir/../build/$helper_name"
    elif [[ -x "${HOME:-}/.local/bin/$helper_name" ]]; then
        printf '%s\n' "$HOME/.local/bin/$helper_name"
    else
        return 1
    fi
}

learning_panel=$(helper_path tipe-learning-panel) || {
    echo "找不到 tipe-learning-panel 辅助程序" >&2
    exit 1
}

args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            echo "未知参数：$1" >&2
            exit 2
            ;;
        *)
            args+=("$1")
            shift
            ;;
    esac
done

wait_args=()
if [[ "${#args[@]}" -eq 0 && -n "${HOME:-}" ]]; then
    live_request="${XDG_CACHE_HOME:-$HOME/.cache}/tipe/supervision-current.tsv"
    last_request="${XDG_CACHE_HOME:-$HOME/.cache}/tipe/supervision-last.tsv"
    args+=("$live_request")
    wait_args+=(--wait-missing --fallback-request "$last_request")
fi

exec "$learning_panel" --window --window-title "TiPE" --replay --defer-replay --explain-output --learn-output "${wait_args[@]}" "${args[@]}"
