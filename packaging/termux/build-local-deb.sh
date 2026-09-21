#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="${MILENA_VERSION:-0.1.1}"
VERSION="${VERSION#v}"
PREFIX_DIR="${PREFIX:-}"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT_DIR" log -1 --format=%ct 2>/dev/null || printf '0')}"

if [[ -z "$PREFIX_DIR" || "$PREFIX_DIR" != */usr || "$PREFIX_DIR" == *[[:space:]]* ]]; then
    echo 'Run this script inside Termux with PREFIX pointing to .../usr' >&2
    exit 1
fi
if [[ ! "$VERSION" =~ ^[0-9][0-9A-Za-z.+:~-]*$ ]]; then
    echo "MILENA_VERSION no es válida para Debian: $VERSION" >&2
    exit 1
fi
if [[ ! "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]]; then
    echo 'SOURCE_DATE_EPOCH debe ser un entero no negativo' >&2
    exit 1
fi
for command in clang make dpkg dpkg-deb install sha256sum; do
    command -v "$command" >/dev/null 2>&1 || { echo "Missing required command: $command" >&2; exit 1; }
done
cd "$ROOT_DIR"
make clean
CC=clang make
CC=clang make test
ARCH="$(dpkg --print-architecture)"
[[ "$ARCH" == 'aarch64' ]] || { echo "Termux package target must be aarch64 (got $ARCH)" >&2; exit 1; }
DIST_DIR="$ROOT_DIR/dist/termux"
STAGE="$DIST_DIR/stage"
rm -rf "$STAGE" "$DIST_DIR"/milena_*.deb "$DIST_DIR"/milena_*.deb.sha256
mkdir -p "$STAGE/${PREFIX_DIR#/}/bin" "$STAGE/${PREFIX_DIR#/}/share/doc/milena" "$STAGE/DEBIAN" "$DIST_DIR"
trap 'rm -rf "$STAGE"' EXIT
install -m 0755 milena "$STAGE/${PREFIX_DIR#/}/bin/milena"
install -m 0644 README.md "$STAGE/${PREFIX_DIR#/}/share/doc/milena/README.md"
cp -R examples "$STAGE/${PREFIX_DIR#/}/share/doc/milena/"
cat > "$STAGE/DEBIAN/control" <<EOF_CONTROL
Package: milena
Version: $VERSION
Section: science
Priority: optional
Architecture: aarch64
Maintainer: Milena SST <maintainers@milena.invalid>
Description: Milena SST data analysis language
 Milena analyzes occupational safety and health data.
EOF_CONTROL
# Normalize ownership, modes and timestamps so identical inputs produce identical archives.
find "$STAGE" -type d -exec chmod 0755 {} +
find "$STAGE" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGE/${PREFIX_DIR#/}/bin/milena"
find "$STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
OUTPUT="$DIST_DIR/milena_${VERSION}_aarch64.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$OUTPUT" >/dev/null
dpkg-deb --info "$OUTPUT" >/dev/null
dpkg-deb --contents "$OUTPUT" | grep -Fq "${PREFIX_DIR#/}/bin/milena"
dpkg-deb -f "$OUTPUT" Package | grep -Fxq milena
dpkg-deb -f "$OUTPUT" Architecture | grep -Fxq aarch64
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
printf 'Package created: %s\nChecksum created: %s\n' "$OUTPUT" "$OUTPUT.sha256"
