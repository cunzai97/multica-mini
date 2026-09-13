#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$ROOT/bin/multica-core" serve --data-dir "$ROOT/data" "$@"
