#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

usage() {
    cat <<'EOF'
用法：tipe-analyze-window [--learn-output] [--run-on-open] [--preferences PATH] [--command CMD] [--config PATH] [REQUEST_TSV]

打开 TiPE 学习/调试窗口，进入点击式模型分析模式。
这个工具不会重启 fcitx5，不会切换输入法，也不会修改 profile 文件。

默认是只读点击模式：窗口显示请求内容，打开时不调用模型，
只有点击“分析”时才会运行一次当前配置的模型。
如果确实想恢复旧的打开即分析行为，可以加 --run-on-open。
不传 REQUEST_TSV 时，默认读取当前 TiPE 实时监督快照。
如果当前还没有活动拼音，窗口会立即打开，可以先显示上一次捕获的请求，
并在新的 TiPE 输入出现后自动刷新。
需要把模型返回的安全结果写入 TiPE 偏好时，使用 --learn-output。
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

args=(--window --window-title "TiPE 分析窗口" --replay --explain-output)
request_path=""
run_on_open=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run-on-open)
            run_on_open=1
            shift
            ;;
        --learn-output)
            args+=(--learn-output)
            shift
            ;;
        --preferences|--command|--config)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "$1 需要一个值" >&2
                exit 2
            fi
            args+=("$1" "$2")
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            echo "未知参数：$1" >&2
            exit 2
            ;;
        *)
            if [[ -n "$request_path" ]]; then
                usage >&2
                exit 2
            fi
            request_path="$1"
            shift
            ;;
    esac
done

if [[ "$run_on_open" != 1 ]]; then
    args+=(--defer-replay)
fi

if [[ -n "$request_path" ]]; then
    args+=("$request_path")
elif [[ -n "${HOME:-}" ]]; then
    live_request="${XDG_CACHE_HOME:-$HOME/.cache}/tipe/supervision-current.tsv"
    last_request="${XDG_CACHE_HOME:-$HOME/.cache}/tipe/supervision-last.tsv"
    if [[ -r "$live_request" ]]; then
        args+=("$live_request")
    else
        args+=(--wait-missing --fallback-request "$last_request" "$live_request")
    fi
fi

exec "$learning_panel" "${args[@]}"
