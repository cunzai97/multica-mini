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

wait_status() {
    issue=$1
    expected=$2
    for attempt in $(seq 1 100); do
        current=$(curl -fsS "$BASE/api/issues/$issue" | python3 -c 'import json,sys; print(json.load(sys.stdin)["status"])')
        [ "$current" = "$expected" ] && return 0
        sleep 0.1
    done
    echo "issue $issue did not reach $expected" >&2
    curl -fsS "$BASE/api/issues/$issue" >&2
    return 1
}

curl -fsS "$BASE/" | grep '本地协作台' >/dev/null
curl -fsS "$BASE/" | grep 'theme-control' >/dev/null
curl -fsS "$BASE/app.css" | grep -- '--sidebar-width' >/dev/null
curl -fsS -X POST "$BASE/api/agents" -H 'Content-Type: application/json' \
    --data "{\"id\":\"leader\",\"name\":\"Leader\",\"command\":[\"$FAKE\",\"{prompt}\"],\"skills\":[]}" >/dev/null
curl -fsS -X POST "$BASE/api/agents" -H 'Content-Type: application/json' \
    --data "{\"id\":\"worker\",\"name\":\"Worker\",\"role\":\"implementation\",\"command\":[\"$FAKE\",\"{prompt}\"],\"skills\":[]}" >/dev/null
curl -fsS "$BASE/api/agents/worker" | grep '"role":"implementation"' >/dev/null
curl -fsS -X PUT "$BASE/api/agents/worker" -H 'Content-Type: application/json' \
    --data "{\"name\":\"Worker Updated\",\"role\":\"implementation and verification\",\"command\":[\"$FAKE\",\"{prompt}\"],\"skills\":[\"/tmp/demo/SKILL.md\"],\"enabled\":true}" |
    grep '"name":"Worker Updated"' >/dev/null
curl -fsS -X POST "$BASE/api/agents/worker/test" -H 'Content-Type: application/json' \
    --data "{\"cwd\":\"$TMP/work\"}" | grep '"ok":true' >/dev/null
curl -fsS -X POST "$BASE/api/squads" -H 'Content-Type: application/json' \
    --data '{"id":"team","name":"Team","leader_id":"leader","members":[{"agent_id":"worker","role":"implementation"}]}' >/dev/null
curl -fsS "$BASE/api/squads/team" | grep '"leader_id":"leader"' >/dev/null
curl -fsS -X PUT "$BASE/api/squads/team" -H 'Content-Type: application/json' \
    --data '{"name":"Team Updated","leader_id":"leader","members":[{"agent_id":"worker","role":"implementation and verification"}]}' |
    grep '"name":"Team Updated"' >/dev/null

issue_id=$(curl -fsS -X POST "$BASE/api/issues" -H 'Content-Type: application/json' \
    --data "{\"title\":\"Web API smoke\",\"description\":\"Run the complete collaboration loop.\",\"assignee_type\":\"squad\",\"assignee_id\":\"team\",\"cwd\":\"$TMP/work\",\"review_policy\":\"auto\",\"timeout_seconds\":30,\"max_retries\":0}" |
    sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
test -n "$issue_id"
curl -fsS -X PUT "$BASE/api/issues/$issue_id" -H 'Content-Type: application/json' \
    --data "{\"title\":\"Web API smoke updated\",\"description\":\"Run the complete collaboration loop.\",\"assignee_type\":\"squad\",\"assignee_id\":\"team\",\"cwd\":\"$TMP/work\",\"review_policy\":\"auto\",\"timeout_seconds\":30,\"max_retries\":0}" |
    grep '"title":"Web API smoke updated"' >/dev/null
curl -fsS -X POST "$BASE/api/issues/$issue_id/comments" -H 'Content-Type: application/json' \
    --data '{"content":"Human acceptance criteria."}' | grep 'Human acceptance criteria' >/dev/null
curl -fsS -X POST "$BASE/api/issues/$issue_id/run" -H 'Content-Type: application/json' \
    --data '{"max_runs":4}' | grep '"accepted":true' >/dev/null
wait_status "$issue_id" done
curl -fsS "$BASE/api/state" | grep 'worker completed with verification' >/dev/null
curl -fsS "$BASE/api/state" | grep 'implementation and verification' >/dev/null
curl -fsS "$BASE/api/issues/$issue_id" | grep '"prompt":' >/dev/null

manual_id=$(curl -fsS -X POST "$BASE/api/issues" -H 'Content-Type: application/json' \
    --data "{\"title\":\"Manual review\",\"description\":\"Require review.\",\"assignee_type\":\"agent\",\"assignee_id\":\"worker\",\"cwd\":\"$TMP/work\",\"review_policy\":\"manual\"}" |
    sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
curl -fsS -X POST "$BASE/api/issues/$manual_id/run" -H 'Content-Type: application/json' --data '{}' >/dev/null
wait_status "$manual_id" in_review
curl -fsS -X POST "$BASE/api/issues/$manual_id/comments" -H 'Content-Type: application/json' \
    --data '{"content":"Please revise the result."}' | grep 'Please revise the result.' >/dev/null
curl -fsS -X DELETE "$BASE/api/issues/$manual_id" | grep "\"deleted\":\"$manual_id\"" >/dev/null
if curl -fsS "$BASE/api/issues/$manual_id" >/dev/null 2>&1; then
    echo "deleted issue should not be readable" >&2
    exit 1
fi

if curl -fsS -X DELETE "$BASE/api/agents/worker" >/dev/null 2>&1; then
    echo "referenced agent deletion should fail" >&2
    exit 1
fi
if curl -fsS -X DELETE "$BASE/api/squads/team" >/dev/null 2>&1; then
    echo "referenced squad deletion should fail" >&2
    exit 1
fi
curl -fsS -X POST "$BASE/api/agents" -H 'Content-Type: application/json' \
    --data "{\"id\":\"unused\",\"name\":\"Unused\",\"command\":[\"$FAKE\",\"{prompt}\"],\"skills\":[]}" >/dev/null
curl -fsS -X DELETE "$BASE/api/agents/unused" | grep '"deleted":"unused"' >/dev/null

curl -fsS "$BASE/api/export" > "$TMP/export.json"
grep '"format": "multica-mini-backup"' "$TMP/export.json" >/dev/null
curl -fsS -X POST "$BASE/api/import" -H 'Content-Type: application/json' --data-binary @"$TMP/export.json" |
    grep '"imported":true' >/dev/null

printf 'multica-core Web/API smoke test passed\n'
