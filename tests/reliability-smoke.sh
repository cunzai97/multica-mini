#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/bin/multica-core"
FAKE="$ROOT/tests/fake-agent.sh"
TMP=/tmp/multica-core-reliability-$$
PORT=$((41000 + ($$ % 15000)))
BASE="http://127.0.0.1:$PORT"
SERVER_PID=

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

start_server() {
    "$BIN" serve --data-dir "$TMP/data" --port "$PORT" --web-root "$ROOT/web" >"$TMP/server.log" 2>&1 &
    SERVER_PID=$!
    for attempt in $(seq 1 50); do
        curl -fsS "$BASE/api/health" >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    cat "$TMP/server.log" >&2
    return 1
}

wait_status() {
    issue=$1
    expected=$2
    for attempt in $(seq 1 150); do
        current=$(curl -fsS "$BASE/api/issues/$issue" | python3 -c 'import json,sys; print(json.load(sys.stdin)["status"])')
        [ "$current" = "$expected" ] && return 0
        sleep 0.1
    done
    echo "issue $issue did not reach $expected" >&2
    curl -fsS "$BASE/api/issues/$issue" >&2
    return 1
}

mkdir -p "$TMP/work"
chmod +x "$FAKE"
start_server

for id in flaky sleeper; do
    curl -fsS -X POST "$BASE/api/agents" -H 'Content-Type: application/json' \
        --data "{\"id\":\"$id\",\"name\":\"$id\",\"command\":[\"$FAKE\",\"{prompt}\"],\"skills\":[]}" >/dev/null
done

retry_id=$(curl -fsS -X POST "$BASE/api/issues" -H 'Content-Type: application/json' \
    --data "{\"title\":\"Retry\",\"description\":\"Fail once.\",\"assignee_type\":\"agent\",\"assignee_id\":\"flaky\",\"cwd\":\"$TMP/work\",\"max_retries\":1,\"timeout_seconds\":10}" |
    sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
curl -fsS -X POST "$BASE/api/issues/$retry_id/run" -H 'Content-Type: application/json' --data '{}' >/dev/null
wait_status "$retry_id" done
curl -fsS "$BASE/api/issues/$retry_id" >"$TMP/retry.json"
test "$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["runs"]))' "$TMP/retry.json")" -eq 2
grep 'flaky agent recovered on retry' "$TMP/retry.json" >/dev/null

timeout_id=$(curl -fsS -X POST "$BASE/api/issues" -H 'Content-Type: application/json' \
    --data "{\"title\":\"Timeout\",\"description\":\"Must time out.\",\"assignee_type\":\"agent\",\"assignee_id\":\"sleeper\",\"cwd\":\"$TMP/work\",\"timeout_seconds\":1}" |
    sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
curl -fsS -X POST "$BASE/api/issues/$timeout_id/run" -H 'Content-Type: application/json' --data '{}' >/dev/null
wait_status "$timeout_id" failed
curl -fsS "$BASE/api/issues/$timeout_id" | grep '"failure_reason":"timeout"' >/dev/null

cancel_id=$(curl -fsS -X POST "$BASE/api/issues" -H 'Content-Type: application/json' \
    --data "{\"title\":\"Cancel\",\"description\":\"Must be cancelled.\",\"assignee_type\":\"agent\",\"assignee_id\":\"sleeper\",\"cwd\":\"$TMP/work\",\"timeout_seconds\":30}" |
    sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
curl -fsS -X POST "$BASE/api/issues/$cancel_id/run" -H 'Content-Type: application/json' --data '{}' >/dev/null
wait_status "$cancel_id" in_progress
curl -fsS -X POST "$BASE/api/issues/$cancel_id/cancel" -H 'Content-Type: application/json' --data '{}' |
    grep '"cancel_requested":true' >/dev/null
wait_status "$cancel_id" cancelled

restart_id=$(curl -fsS -X POST "$BASE/api/issues" -H 'Content-Type: application/json' \
    --data "{\"title\":\"Restart recovery\",\"description\":\"Must be recovered after restart.\",\"assignee_type\":\"agent\",\"assignee_id\":\"sleeper\",\"cwd\":\"$TMP/work\",\"timeout_seconds\":30}" |
    sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
curl -fsS -X POST "$BASE/api/issues/$restart_id/run" -H 'Content-Type: application/json' --data '{}' >/dev/null
wait_status "$restart_id" in_progress
kill -9 "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=
start_server
wait_status "$restart_id" failed
curl -fsS "$BASE/api/issues/$restart_id" >"$TMP/restarted.json"
grep 'service restarted while the task was running' "$TMP/restarted.json" >/dev/null
grep '"failure_reason":"service_restarted"' "$TMP/restarted.json" >/dev/null

printf 'multica-core reliability smoke test passed\n'
