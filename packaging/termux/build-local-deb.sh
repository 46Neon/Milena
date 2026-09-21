#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
umask 022

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="${MILENA_VERSION:-0.1.1}"
VERSION="${VERSION#v}"
PREFIX_DIR="${PREFIX:-}"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT_DIR" log -1 --format=%ct 2>/dev/null || printf '0')}"

[[ "$PREFIX_DIR" =~ ^/data/data/[A-Za-z0-9._-]+/files/usr$ ]] || {
    echo 'Run this script inside Termux with PREFIX pointing to .../files/usr' >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9][0-9A-Za-z.+:~-]*$ ]] || { echo "MILENA_VERSION no es válida: $VERSION" >&2; exit 1; }
[[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || { echo 'SOURCE_DATE_EPOCH debe ser un entero no negativo' >&2; exit 1; }
for command in clang make dpkg dpkg-deb install sha256sum readelf python3; do
    command -v "$command" >/dev/null 2>&1 || { echo "Missing required command: $command" >&2; exit 1; }
done
cd "$ROOT_DIR"
[[ -z "$(git status --porcelain --untracked-files=all)" ]] || { echo 'workspace is not clean' >&2; exit 1; }
ARCH="$(dpkg --print-architecture)"
[[ "$ARCH" == 'aarch64' ]] || { echo "Termux package target must be aarch64 (got $ARCH)" >&2; exit 1; }
TARGET_TRIPLE="$(clang -print-target-triple 2>/dev/null || true)"
printf '%s' "$TARGET_TRIPLE" | grep -Eqi aarch64 || { echo 'clang target is not aarch64' >&2; exit 1; }

make clean
CC=clang make
CC=clang make test
# Check the produced ELF before it is copied into a package.
readelf -h milena | grep -Eq 'Class:[[:space:]]+ELF64'
readelf -h milena | grep -Eq 'Machine:[[:space:]]+AArch64'
if readelf -d milena 2>/dev/null | grep -Eiq 'libc6|libstdc\+\+|libgcc_s\.so'; then
    echo 'ELF contains a Debian/Ubuntu runtime dependency' >&2; exit 1
fi

DIST_DIR="$ROOT_DIR/dist/termux"
STAGE="$DIST_DIR/stage"
rm -rf "$STAGE" "$DIST_DIR"/milena_*.deb "$DIST_DIR"/milena_*.sha256 "$DIST_DIR"/*.json "$DIST_DIR"/*.txt
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
find "$STAGE" -type d -exec chmod 0755 {} +
find "$STAGE" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGE/${PREFIX_DIR#/}/bin/milena"
find "$STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
OUTPUT="$DIST_DIR/milena_${VERSION}_aarch64.deb"
dpkg-deb --build --root-owner-group -Zxz --uniform-compression "$STAGE" "$OUTPUT" >/dev/null
dpkg-deb -f "$OUTPUT" Package | grep -Fxq milena
dpkg-deb -f "$OUTPUT" Architecture | grep -Fxq aarch64
dpkg-deb --contents "$OUTPUT" | grep -Fq "${PREFIX_DIR#/}/bin/milena"
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
python3 - "$OUTPUT" "$OUTPUT.sha256" "$ROOT_DIR" "$SOURCE_DATE_EPOCH" "$TARGET_TRIPLE" > "$OUTPUT.provenance.json" <<'PY'
import hashlib, json, os, subprocess, sys
package, checksum, root, epoch, triple = sys.argv[1:]
version = subprocess.check_output(['dpkg-deb','-f',package,'Version'], text=True).strip()
data = {
  'schema': 'milena.termux.provenance.v1',
  'artifact': {'filename': os.path.basename(package), 'sha256': hashlib.sha256(open(package,'rb').read()).hexdigest(), 'architecture': 'aarch64', 'version': version},
  'source': {'commit': subprocess.check_output(['git','-C',root,'rev-parse','HEAD'], text=True).strip(), 'repository': '46Neon/Milena'},
  'build': {'source_date_epoch': int(epoch), 'compiler': subprocess.check_output(['clang','--version'], text=True).splitlines()[0], 'target': triple, 'runner': 'termux-aarch64'},
  'reproducibility': {'normalized_timestamps': True, 'normalized_ownership': True}
}
print(json.dumps(data, sort_keys=True, indent=2))
PY
python3 - "$OUTPUT" "$OUTPUT.sbom.json" "$SOURCE_DATE_EPOCH" <<'PY'
import hashlib, json, os, subprocess, sys
package, out, epoch = sys.argv[1:]
entries=[]
for line in subprocess.check_output(['dpkg-deb','--contents',package], text=True).splitlines():
    parts=line.split(maxsplit=5)
    if len(parts)==6 and parts[5].strip('./') not in ('','.'):
        entries.append({'path': parts[5].removeprefix('./'), 'type': 'file' if parts[1].startswith('-') else 'directory'})
print(json.dumps({'bomFormat':'CycloneDX','specVersion':'1.5','serialNumber':'urn:uuid:milena-'+hashlib.sha256(open(package,'rb').read()).hexdigest()[:32], 'metadata':{'timestamp':f'{int(epoch)}','component':{'name':'milena','version':subprocess.check_output(['dpkg-deb','-f',package,'Version'],text=True).strip(),'type':'application','purl':'pkg:generic/milena'}},'components':entries}, sort_keys=True, indent=2), file=open(out,'w'))
PY
sha256sum "$OUTPUT" "$OUTPUT.sha256" "$DIST_DIR"/*.json > "$DIST_DIR/SHA256SUMS"
printf 'Package created: %s\nEvidence: %s\n' "$OUTPUT" "$DIST_DIR/SHA256SUMS"
