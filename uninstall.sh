#!/bin/sh
set -eu

PREFIX=${HOME:?HOME is required}/.local
DATA_DIR=${XDG_DATA_HOME:-"$HOME/.local/share"}/multica-core
REMOVE_DATA=false

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX=$2; shift 2 ;;
        --data-dir) DATA_DIR=$2; shift 2 ;;
        --remove-data) REMOVE_DATA=true; shift ;;
        -h|--help)
            echo "usage: ./uninstall.sh [--prefix DIR] [--data-dir DIR] [--remove-data]"
            exit 0
            ;;
        *) echo "uninstall.sh: unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -n "$PREFIX" ] && [ "$PREFIX" != / ] || { echo "unsafe prefix" >&2; exit 2; }
[ -n "$DATA_DIR" ] && [ "$DATA_DIR" != / ] || { echo "unsafe data directory" >&2; exit 2; }

if [ -x "$PREFIX/bin/multica-core-service" ]; then
    "$PREFIX/bin/multica-core-service" stop --data-dir "$DATA_DIR" 2>/dev/null || true
fi
rm -f "$PREFIX/bin/multica-core" "$PREFIX/bin/multica-core-service" "$PREFIX/bin/multica-core-uninstall"
rm -rf "$PREFIX/share/multica-core"
if $REMOVE_DATA; then rm -rf "$DATA_DIR"; fi

echo "Multica Mini removed from $PREFIX"
if ! $REMOVE_DATA; then echo "Data preserved at $DATA_DIR"; fi
