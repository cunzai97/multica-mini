#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/bin/multica-core"
FAKE="$ROOT/tests/fake-agent.sh"
TMP=${TMPDIR:-/tmp}/multica-core-smoke-$$
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

mkdir -p "$TMP/work"
chmod +x "$FAKE"

"$BIN" init --data-dir "$TMP/data" >/dev/null
"$BIN" agent add leader --data-dir "$TMP/data" --name Leader --exec "$FAKE" --arg '{prompt}' >/dev/null
"$BIN" agent add worker --data-dir "$TMP/data" --name Worker --exec "$FAKE" --arg '{prompt}' --role implementation >/dev/null
"$BIN" squad create team --data-dir "$TMP/data" --name Team --leader leader >/dev/null
"$BIN" squad member-add team worker --data-dir "$TMP/data" --role implementation >/dev/null

issue_id=$("$BIN" issue create --data-dir "$TMP/data" \
    --title 'Smoke task' \
    --description 'Exercise leader dispatch and completion.' \
    --assignee squad:team \
    --cwd "$TMP/work")

"$BIN" run "$issue_id" --data-dir "$TMP/data" --max-runs 4 >/dev/null
result=$("$BIN" issue show "$issue_id" --data-dir "$TMP/data")

printf '%s\n' "$result" | grep '"status": "in_review"' >/dev/null
printf '%s\n' "$result" | grep 'worker completed with verification' >/dev/null
test "$(find "$TMP/data/runs" -type f -name '*.json' | wc -l)" -eq 3

printf 'multica-core smoke test passed\n'
