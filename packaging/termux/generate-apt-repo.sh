#!/usr/bin/env bash
set -euo pipefail

# Ejecutar en Linux/CI, no dentro de una instalación Termux mínima.
# Requiere: dpkg-deb, dpkg-scanpackages, apt-ftparchive y gpg.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RAW_KEY_ID="${MILENA_GPG_KEY_ID:?Define MILENA_GPG_KEY_ID con la clave de publicación}"
KEY_ID="$(printf '%s' "$RAW_KEY_ID" | tr -d '[:space:]')"
KEY_ID="${KEY_ID#rsa3072/}"
KEY_ID="${KEY_ID#0x}"
if [[ ! "$KEY_ID" =~ ^[[:xdigit:]]{8,64}$ ]]; then
    echo 'MILENA_GPG_KEY_ID no contiene un identificador hexadecimal válido' >&2
    exit 1
fi
INPUT_DIR="${1:-$ROOT_DIR/dist/termux}"
OUTPUT_DIR="${2:-$ROOT_DIR/dist/apt}"

for command in dpkg-deb dpkg-scanpackages apt-ftparchive gpg sha256sum; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Falta el comando requerido: $command" >&2
        exit 1
    }
done

shopt -s nullglob
packages=("$INPUT_DIR"/milena_*.deb)
if (( ${#packages[@]} != 1 )); then
    echo "Se requiere exactamente un paquete Milena Termux aarch64 (encontrados: ${#packages[@]})" >&2
    exit 1
fi
package="${packages[0]}"
PACKAGE_NAME="$(dpkg-deb -f "$package" Package)"
PACKAGE_ARCH="$(dpkg-deb -f "$package" Architecture)"
PACKAGE_VERSION="$(dpkg-deb -f "$package" Version)"
[[ "$PACKAGE_NAME" == milena ]] || { echo 'El artefacto no es el paquete milena' >&2; exit 1; }
[[ "$PACKAGE_ARCH" == aarch64 ]] || { echo 'Solo se publica Termux aarch64; se rechazó otra arquitectura' >&2; exit 1; }
[[ -n "$PACKAGE_VERSION" && "$PACKAGE_VERSION" != *[[:space:]]* ]] || { echo 'El paquete no tiene una versión válida' >&2; exit 1; }
if dpkg-deb --contents "$package" | grep -Eq '(^|[[:space:]])(usr|bin|lib|etc)/'; then
    echo 'El paquete contiene rutas de Debian y no puede entrar al repositorio Termux' >&2
    exit 1
fi
VERSION="${MILENA_VERSION:-$PACKAGE_VERSION}"
VERSION="${VERSION#v}"
if [[ "$VERSION" != "$PACKAGE_VERSION" ]]; then
    echo "MILENA_VERSION ($VERSION) no coincide con la versión del paquete ($PACKAGE_VERSION)" >&2
    exit 1
fi

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR/pool/main/m/milena" "$OUTPUT_DIR/dists/stable/main/binary-aarch64"
cp "$package" "$OUTPUT_DIR/pool/main/m/milena/"
sha256sum "$package" > "$OUTPUT_DIR/pool/main/m/milena/$(basename "$package").sha256"
(
    cd "$OUTPUT_DIR"
    dpkg-scanpackages -a aarch64 pool /dev/null > dists/stable/main/binary-aarch64/Packages
)
gzip -9n < "$OUTPUT_DIR/dists/stable/main/binary-aarch64/Packages" > "$OUTPUT_DIR/dists/stable/main/binary-aarch64/Packages.gz"

RELEASE_CONFIG="$OUTPUT_DIR/.apt-ftparchive.conf"
cat > "$RELEASE_CONFIG" <<'EOF_CONFIG'
APT::FTPArchive::Release::Origin "Milena";
APT::FTPArchive::Release::Label "Milena APT";
APT::FTPArchive::Release::Suite "stable";
APT::FTPArchive::Release::Codename "stable";
APT::FTPArchive::Release::Components "main";
APT::FTPArchive::Release::Architectures "aarch64";
EOF_CONFIG
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"
[[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || { echo 'SOURCE_DATE_EPOCH debe ser un entero no negativo' >&2; exit 1; }
export SOURCE_DATE_EPOCH
apt-ftparchive -c="$RELEASE_CONFIG" release "$OUTPUT_DIR/dists/stable" > "$OUTPUT_DIR/dists/stable/Release"
rm -f "$RELEASE_CONFIG"

GPG_ARGS=(--batch --yes --local-user "$KEY_ID")
PASSPHRASE_FILE=""
cleanup() { [[ -z "$PASSPHRASE_FILE" ]] || rm -f "$PASSPHRASE_FILE"; }
trap cleanup EXIT
if [[ -n "${MILENA_GPG_PASSPHRASE:-}" ]]; then
    umask 077
    PASSPHRASE_FILE="$(mktemp)"
    printf '%s' "$MILENA_GPG_PASSPHRASE" > "$PASSPHRASE_FILE"
    GPG_ARGS+=(--pinentry-mode loopback --passphrase-file "$PASSPHRASE_FILE")
fi
gpg "${GPG_ARGS[@]}" --clearsign \
    --output "$OUTPUT_DIR/dists/stable/InRelease" "$OUTPUT_DIR/dists/stable/Release"
gpg "${GPG_ARGS[@]}" --armor --detach-sign \
    --output "$OUTPUT_DIR/dists/stable/Release.gpg" "$OUTPUT_DIR/dists/stable/Release"
rm -f "$PASSPHRASE_FILE"
PASSPHRASE_FILE=""

printf 'Repositorio APT Termux aarch64 generado en %s\n' "$OUTPUT_DIR"
