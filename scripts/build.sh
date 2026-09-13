#!/bin/sh
set -eu

PROJECT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORKSPACE=$(CDPATH= cd -- "$PROJECT/.." && pwd)
DEPS="$PROJECT/vendor"
[ -f "$DEPS/nlohmann/json.hpp" ] || DEPS="$WORKSPACE/vendor"

if [ -z "${CXX:-}" ]; then
    if [ -x "$PROJECT/toolchain/bin/x86_64-linux-musl-g++" ]; then
        CXX="$PROJECT/toolchain/bin/x86_64-linux-musl-g++"
    elif [ -x "$WORKSPACE/toolchain/bin/x86_64-linux-musl-g++" ]; then
        CXX="$WORKSPACE/toolchain/bin/x86_64-linux-musl-g++"
    elif command -v x86_64-linux-musl-g++ >/dev/null 2>&1; then
        CXX=$(command -v x86_64-linux-musl-g++)
    else
        CXX=$(command -v c++ || true)
    fi
fi
if [ -z "${STRIP:-}" ]; then
    STRIP=$(command -v x86_64-linux-musl-strip 2>/dev/null || command -v strip 2>/dev/null || true)
fi
OUT="$PROJECT/bin/multica-core"

[ -n "$CXX" ] || { echo "C++17 compiler not found; set CXX" >&2; exit 1; }
[ -f "$DEPS/cpp-httplib/httplib.h" ] || { echo "vendored cpp-httplib not found" >&2; exit 1; }
[ -f "$DEPS/nlohmann/json.hpp" ] || { echo "vendored nlohmann/json not found" >&2; exit 1; }
mkdir -p "$PROJECT/bin"

"$CXX" \
    -O2 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -static \
    -std=c++17 \
    -pthread \
    -I"$DEPS/cpp-httplib" \
    -I"$DEPS/nlohmann" \
    "$PROJECT/src/main.cpp" \
    "$DEPS/cpp-httplib/httplib.cpp" \
    -o "$OUT"

if [ -n "$STRIP" ]; then "$STRIP" "$OUT"; fi
chmod 0755 "$OUT"
printf 'Built %s\n' "$OUT"
