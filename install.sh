#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX=${HOME:?HOME is required}/.local
DATA_DIR=${XDG_DATA_HOME:-"$HOME/.local/share"}/multica-core

usage() {
    cat <<'EOF'
usage: ./install.sh [--prefix DIR] [--data-dir DIR]
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX=$2; shift 2 ;;
        --data-dir) DATA_DIR=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "install.sh: unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -n "$PREFIX" ] && [ "$PREFIX" != / ] || { echo "unsafe prefix" >&2; exit 2; }
[ -n "$DATA_DIR" ] && [ "$DATA_DIR" != / ] || { echo "unsafe data directory" >&2; exit 2; }
[ -x "$ROOT/bin/multica-core" ] || { echo "bin/multica-core is missing" >&2; exit 1; }
[ -f "$ROOT/web/index.html" ] || { echo "web/index.html is missing" >&2; exit 1; }

SHARE_DIR="$PREFIX/share/multica-core"
mkdir -p "$PREFIX/bin" "$SHARE_DIR" "$DATA_DIR"
install -m 0755 "$ROOT/bin/multica-core" "$PREFIX/bin/multica-core"
rm -rf "$SHARE_DIR/web"
cp -R "$ROOT/web" "$SHARE_DIR/web"
if [ -d "$ROOT/docs" ]; then
    rm -rf "$SHARE_DIR/docs"
    cp -R "$ROOT/docs" "$SHARE_DIR/docs"
fi
for document in README.zh-CN.md LICENSE NOTICE THIRD_PARTY_LICENSES; do
    if [ -f "$ROOT/$document" ]; then
        install -m 0644 "$ROOT/$document" "$SHARE_DIR/$document"
    fi
done
"$PREFIX/bin/multica-core" init --data-dir "$DATA_DIR" >/dev/null

printf 'Installed: %s/bin/multica-core\n' "$PREFIX"
printf 'Web:       %s/web\n' "$SHARE_DIR"
printf 'Data:      %s\n' "$DATA_DIR"
printf 'Start:     %s/bin/multica-core serve --data-dir %s\n' "$PREFIX" "$DATA_DIR"
