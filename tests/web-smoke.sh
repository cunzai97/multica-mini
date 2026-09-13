#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/bin/multica-core"
FAKE="$ROOT/tests/fake-agent.sh"
TMP=/tmp/multica-core-web-smoke-$$
PORT=$((32000 + ($$ % 20000)))
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

mkdir -p "$TMP/work"
chmod +x "$FAKE"
"$BIN" serve --data-dir "$TMP/data" --port "$PORT" --web-root "$ROOT/web" >"$TMP/server.log" 2>&1 &
SERVER_PID=$!

ready=false
for attempt in $(seq 1 50); do
    if curl -fsS "$BASE/api/health" >/dev/null 2>&1; then ready=true; break; fi
    sleep 0.1
done
$ready || { cat "$TMP/server.log" >&2; echo "server did not start" >&2; exit 1; }

curl -fsS "$BASE/" | grep '本地协作台' >/dev/null
curl -fsS "$BASE/" | grep 'theme-control' >/dev/null
curl -fsS "$BASE/app.css" | grep -- '--sidebar-width' >/dev/null
curl -fsS -X POST "$BASE/api/agents" -H 'Content-Type: application/json' \
    --data "{\"id\":\"leader\",\"name\":\"Leader\",\"command\":[\"$FAKE\",\"{prompt}\"],\"skills\":[]}" >/dev/null
curl -fsS -X POST "$BASE/api/agents" -H 'Content-Type: application/json' \
    --data "{\"id\":\"worker\",\"name\":\"Worker\",\"role\":\"implementation\",\"command\":[\"$FAKE\",\"{prompt}\"],\"skills\":[]}" >/dev/null
curl -fsS "$BASE/api/agents/worker" | grep '"role":"implementation"' >/dev/null
curl -fsS -X PUT "$BASE/api/agents/worker" -H 'Content-Type: application/json' \
    --data "{\"name\":\"Worker Updated\",\"role\":\"implementation and verification\",\"command\":[\"$FAKE\",\"{prompt}\"],\"skills\":[\"/tmp/demo/SKILL.md\"]}" |
    grep '"name":"Worker Updated"' >/dev/null
curl -fsS -X POST "$BASE/api/squads" -H 'Content-Type: application/json' \
    --data '{"id":"team","name":"Team","leader_id":"leader","members":[{"agent_id":"worker","role":"implementation"}]}' >/dev/null

issue_id=$(curl -fsS -X POST "$BASE/api/issues" -H 'Content-Type: application/json' \
    --data "{\"title\":\"Web API smoke\",\"description\":\"Run the complete collaboration loop.\",\"assignee_type\":\"squad\",\"assignee_id\":\"team\",\"cwd\":\"$TMP/work\"}" |
    sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
test -n "$issue_id"
curl -fsS -X POST "$BASE/api/issues/$issue_id/run" -H 'Content-Type: application/json' \
    --data '{"max_runs":4}' | grep '"exit_code":0' >/dev/null
curl -fsS "$BASE/api/issues/$issue_id" | grep '"status":"in_review"' >/dev/null
curl -fsS -X POST "$BASE/api/issues/$issue_id/status" -H 'Content-Type: application/json' \
    --data '{"status":"done"}' | grep '"status":"done"' >/dev/null
curl -fsS "$BASE/api/state" | grep 'worker completed with verification' >/dev/null
curl -fsS "$BASE/api/state" | grep 'implementation and verification' >/dev/null

printf 'multica-core Web/API smoke test passed\n'
