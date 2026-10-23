#!/usr/bin/env bash
set -euo pipefail
trainer="$1"
root="$(mktemp -d "${TMPDIR:-/tmp}/tipe-trainer-test.XXXXXX")"
trap 'rm -rf -- "$root"' EXIT
cat >"$root/input.tsv" <<'EOF'
# local semantic training batch
nihao	你好	editor
woc	我	editor
woc	操	editor
malformed line
EOF
output="$($trainer --state-dir "$root/state" --input "$root/input.tsv")"
grep -q '已提交 3 条，拒绝 1 条' <<<"$output"
test -s "$root/state/model.current"
# A second invocation must load the same durable snapshot and publish a new
# generation through the normal writer path rather than replacing it in place.
$trainer --state-dir "$root/state" --input "$root/input.tsv" >/dev/null
test -s "$root/state/model.previous"
printf 'trainer semantic batch, bounded input and atomic generations passed\n'
