#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -x "$ROOT/bin/multica-core" ]; then
    BIN="$ROOT/bin/multica-core"
    DEFAULT_DATA="$ROOT/data"
else
    BIN="$ROOT/multica-core"
    DEFAULT_DATA=${XDG_DATA_HOME:-"${HOME:?HOME is required}/.local/share"}/multica-core
fi

ACTION=${1:-status}
if [ "$#" -gt 0 ]; then shift; fi
DATA_DIR=$DEFAULT_DATA
PORT=30420

while [ "$#" -gt 0 ]; do
    case "$1" in
        --data-dir) DATA_DIR=$2; shift 2 ;;
        --port) PORT=$2; shift 2 ;;
        *) echo "service.sh: unknown option: $1" >&2; exit 2 ;;
    esac
done

PID_FILE="$DATA_DIR/service.pid"
LOG_FILE="$DATA_DIR/service.log"

running_pid() {
    [ -f "$PID_FILE" ] || return 1
    pid=$(cat "$PID_FILE")
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null || return 1
    printf '%s\n' "$pid"
}

start_service() {
    if pid=$(running_pid); then
        echo "Multica Mini is already running (pid $pid)"
        return 0
    fi
    mkdir -p "$DATA_DIR"
    rm -f "$PID_FILE"
    nohup "$BIN" serve --data-dir "$DATA_DIR" --port "$PORT" >"$LOG_FILE" 2>&1 &
    pid=$!
    printf '%s\n' "$pid" >"$PID_FILE"
    for attempt in $(seq 1 50); do
        if kill -0 "$pid" 2>/dev/null; then
            if command -v curl >/dev/null 2>&1 && curl -fsS "http://127.0.0.1:$PORT/api/health" >/dev/null 2>&1; then
                echo "Multica Mini started (pid $pid): http://127.0.0.1:$PORT"
                return 0
            fi
            if ! command -v curl >/dev/null 2>&1 && [ "$attempt" -ge 3 ]; then
                echo "Multica Mini started (pid $pid): http://127.0.0.1:$PORT"
                return 0
            fi
        else
            cat "$LOG_FILE" >&2
            rm -f "$PID_FILE"
            return 1
        fi
        sleep 0.1
    done
    echo "service did not become ready; see $LOG_FILE" >&2
    return 1
}

stop_service() {
    if ! pid=$(running_pid); then
        rm -f "$PID_FILE"
        echo "Multica Mini is not running"
        return 0
    fi
    kill "$pid"
    for attempt in $(seq 1 50); do
        kill -0 "$pid" 2>/dev/null || { rm -f "$PID_FILE"; echo "Multica Mini stopped"; return 0; }
        sleep 0.1
    done
    kill -9 "$pid" 2>/dev/null || true
    rm -f "$PID_FILE"
    echo "Multica Mini stopped forcibly"
}

case "$ACTION" in
    start) start_service ;;
    stop) stop_service ;;
    restart) stop_service; start_service ;;
    status)
        if pid=$(running_pid); then
            echo "Multica Mini is running (pid $pid): http://127.0.0.1:$PORT"
        else
            echo "Multica Mini is not running"
            exit 1
        fi
        ;;
    *) echo "usage: ./service.sh start|stop|restart|status [--data-dir DIR] [--port PORT]" >&2; exit 2 ;;
esac
