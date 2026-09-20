#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="${MILENA_VERSION:-0.1.1}"
VERSION="${VERSION#v}"
CC_BIN="${CC:-cc}"
ARCH="${MILENA_DEB_ARCH:-$(dpkg --print-architecture)}"
DIST_DIR="$ROOT_DIR/dist/debian"
STAGE="$DIST_DIR/stage"

if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([+~-][0-9A-Za-z.-]+)?$ ]]; then
    printf 'MILENA_VERSION no es válida para Debian: %s\n' "$VERSION" >&2
    exit 1
fi

for command in "$CC_BIN" make dpkg dpkg-deb; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Falta el comando requerido: $command" >&2
        exit 1
    }
done

BASE_CFLAGS="${CFLAGS:--std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -Iinclude}"
VERSION_DEFINE="-DMILENA_VERSION=\\\"$VERSION\\\""
BUILD_CFLAGS="$BASE_CFLAGS $VERSION_DEFINE"

cd "$ROOT_DIR"
make clean
make CC="$CC_BIN" CFLAGS="$BUILD_CFLAGS"
CC="$CC_BIN" make CFLAGS="$BUILD_CFLAGS" test

rm -rf "$STAGE"
mkdir -p "$STAGE/usr/bin" "$STAGE/usr/share/doc/milena"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT
install -m 0755 milena "$STAGE/usr/bin/milena"
install -m 0644 README.md "$STAGE/usr/share/doc/milena/README.md"
cp -R examples "$STAGE/usr/share/doc/milena/"

mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: milena
Version: $VERSION
Section: science
Priority: optional
Architecture: $ARCH
Maintainer: Milena SST <maintainers@milena.invalid>
Description: Milena SST data analysis language
 Milena detects statistical patterns in existing occupational safety and health data.
EOF

find "$STAGE" -type d -exec chmod 0755 {} +
find "$STAGE" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGE/usr/bin/milena"

mkdir -p "$DIST_DIR"
OUTPUT="$DIST_DIR/milena_${VERSION}_${ARCH}.deb"
dpkg-deb --build "$STAGE" "$OUTPUT" >/dev/null
rm -rf "$STAGE"
printf 'Paquete Debian creado: %s\n' "$OUTPUT"
