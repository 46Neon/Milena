#!/usr/bin/env bash
set -euo pipefail

# Ejecutar en Linux/CI, no dentro de una instalación Termux mínima.
# Requiere: dpkg-deb, dpkg-scanpackages, apt-ftparchive y gpg.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="${MANO_VERSION:-0.1.1}"
RAW_KEY_ID="${MANO_GPG_KEY_ID:?Define MANO_GPG_KEY_ID con la clave de publicación}"
KEY_ID="$(printf '%s' "$RAW_KEY_ID" | tr -d '[:space:]')"
KEY_ID="${KEY_ID#rsa3072/}"
KEY_ID="${KEY_ID#0x}"
if [[ ! "$KEY_ID" =~ ^[[:xdigit:]]{8,64}$ ]]; then
    echo "MANO_GPG_KEY_ID no contiene un identificador hexadecimal válido" >&2
    exit 1
fi
INPUT_DIR="${1:-$ROOT_DIR/dist/termux}"
OUTPUT_DIR="${2:-$ROOT_DIR/dist/apt}"

for command in dpkg-deb dpkg-scanpackages apt-ftparchive gpg; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Falta el comando requerido: $command" >&2
        exit 1
    }
done

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR/pool/main/m/milena" "$OUTPUT_DIR/dists/stable/main"

for package in "$INPUT_DIR"/milena_*.deb; do
    [[ -f "$package" ]] || continue
    arch="$(dpkg-deb -f "$package" Architecture)"
    destination="$OUTPUT_DIR/dists/stable/main/binary-$arch"
    mkdir -p "$destination"
    cp "$package" "$OUTPUT_DIR/pool/main/m/milena/"
    # Generar rutas relativas al repositorio; no incluir dist/apt en Filename.
    (
        cd "$OUTPUT_DIR"
        dpkg-scanpackages -a "$arch" pool /dev/null > "dists/stable/main/binary-$arch/Packages"
    )
    gzip -9c "$destination/Packages" > "$destination/Packages.gz"
done

RELEASE_CONFIG="$OUTPUT_DIR/.apt-ftparchive.conf"
cat > "$RELEASE_CONFIG" <<EOF
APT::FTPArchive::Release::Origin "Milena";
APT::FTPArchive::Release::Label "Milena APT";
APT::FTPArchive::Release::Suite "stable";
APT::FTPArchive::Release::Codename "stable";
APT::FTPArchive::Release::Components "main";
APT::FTPArchive::Release::Architectures "aarch64 amd64";
EOF
apt-ftparchive -c="$RELEASE_CONFIG" release "$OUTPUT_DIR/dists/stable" > "$OUTPUT_DIR/dists/stable/Release"
rm -f "$RELEASE_CONFIG"
GPG_ARGS=(--batch --yes --local-user "$KEY_ID")
PASSPHRASE_FILE=""
cleanup_passphrase() { [[ -z "$PASSPHRASE_FILE" ]] || rm -f "$PASSPHRASE_FILE"; }
trap cleanup_passphrase EXIT
if [[ -n "${MANO_GPG_PASSPHRASE:-}" ]]; then
    PASSPHRASE_FILE="$OUTPUT_DIR/.gpg-passphrase"
    umask 077
    printf '%s' "$MANO_GPG_PASSPHRASE" > "$PASSPHRASE_FILE"
    GPG_ARGS+=(--pinentry-mode loopback --passphrase-file "$PASSPHRASE_FILE")
fi
gpg "${GPG_ARGS[@]}" --clearsign \
    --output "$OUTPUT_DIR/dists/stable/InRelease" "$OUTPUT_DIR/dists/stable/Release"
gpg "${GPG_ARGS[@]}" --armor --detach-sign \
    --output "$OUTPUT_DIR/dists/stable/Release.gpg" "$OUTPUT_DIR/dists/stable/Release"
rm -f "$PASSPHRASE_FILE"

printf 'Repositorio APT generado en %s\n' "$OUTPUT_DIR"
