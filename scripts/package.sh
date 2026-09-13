#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$("$ROOT/bin/multica-core" version)
NAME="multica-core-offline-"$VERSION"-linux-x86_64"
DIST="$ROOT/dist"
STAGE="$DIST/$NAME"

[ -x "$ROOT/bin/multica-core" ] || { echo "run scripts/build.sh first" >&2; exit 1; }
for required in web/index.html docs/DESIGN.zh-CN.md start.sh install.sh README.zh-CN.md LICENSE NOTICE THIRD_PARTY_LICENSES; do
    [ -e "$ROOT/$required" ] || { echo "missing $required" >&2; exit 1; }
done

rm -rf "$STAGE"
mkdir -p "$STAGE/bin"
install -m 0755 "$ROOT/bin/multica-core" "$STAGE/bin/multica-core"
cp -R "$ROOT/web" "$STAGE/web"
cp -R "$ROOT/docs" "$STAGE/docs"
install -m 0755 "$ROOT/start.sh" "$ROOT/install.sh" "$STAGE/"
install -m 0644 "$ROOT/README.zh-CN.md" "$ROOT/LICENSE" "$ROOT/NOTICE" \
    "$ROOT/THIRD_PARTY_LICENSES" "$STAGE/"

(
    cd "$STAGE"
    find . -type f ! -name SHA256SUMS -print | LC_ALL=C sort | while IFS= read -r file; do
        sha256sum "$file"
    done > SHA256SUMS
)

tar -C "$DIST" -czf "$DIST/$NAME.tar.gz" "$NAME"
(
    cd "$DIST"
    sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256"
)
rm -rf "$STAGE"
printf 'Packaged %s\n' "$DIST/$NAME.tar.gz"
