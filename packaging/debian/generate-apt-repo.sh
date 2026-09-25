#!/usr/bin/env bash
set -euo pipefail

# Build a signed, static Debian/Ubuntu APT repository from one Milena amd64 .deb.
# This creates local repository metadata only; it does not publish or configure hosts.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INPUT_DIR="${1:-$ROOT_DIR/dist/debian}"
OUTPUT_DIR="${2:-$ROOT_DIR/dist/debian-apt}"
RAW_KEY_ID="${MILENA_GPG_KEY_ID:?Define MILENA_GPG_KEY_ID for the signing key}"
KEY_ID="$(printf '%s' "$RAW_KEY_ID" | tr -d '[:space:]')"
KEY_ID="${KEY_ID#rsa3072/}"
KEY_ID="${KEY_ID#0x}"

if [[ ! "$KEY_ID" =~ ^[[:xdigit:]]{8,64}$ ]]; then
    echo 'MILENA_GPG_KEY_ID must be a hexadecimal key ID or fingerprint' >&2
    exit 1
fi
if [[ ! -d "$INPUT_DIR" ]]; then
    echo "Package input directory does not exist: $INPUT_DIR" >&2
    exit 1
fi
INPUT_DIR="$(cd "$INPUT_DIR" && pwd)"

output_parent="$(dirname "$OUTPUT_DIR")"
mkdir -p "$output_parent"
output_parent="$(cd "$output_parent" && pwd)"
OUTPUT_DIR="$output_parent/$(basename "$OUTPUT_DIR")"
if [[ "$OUTPUT_DIR" == "$INPUT_DIR" || "$OUTPUT_DIR" == "$INPUT_DIR/"* ]]; then
    echo 'APT output directory must be separate from the package input directory' >&2
    exit 1
fi
if [[ -e "$OUTPUT_DIR" || -L "$OUTPUT_DIR" ]]; then
    echo "APT output already exists; choose a new empty path: $OUTPUT_DIR" >&2
    exit 1
fi

for command in dpkg-deb dpkg-scanpackages apt-ftparchive gpg gpgv sha256sum gzip; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Required command not found: $command" >&2
        exit 1
    }
done

shopt -s nullglob
packages=("$INPUT_DIR"/milena_*_amd64.deb)
if (( ${#packages[@]} != 1 )); then
    echo "Expected exactly one Milena amd64 package; found ${#packages[@]}" >&2
    exit 1
fi
package="${packages[0]}"
package_name="$(basename "$package")"
if [[ ! "$package_name" =~ ^milena_[0-9][0-9A-Za-z.+:~-]*_amd64\.deb$ ]]; then
    echo "Invalid Debian package filename: $package_name" >&2
    exit 1
fi
checksum_file="$package.sha256"
if [[ ! -f "$checksum_file" ]]; then
    echo "Missing package checksum: $checksum_file" >&2
    exit 1
fi
read -r expected_hash listed_file < "$checksum_file"
if [[ ! "$expected_hash" =~ ^[[:xdigit:]]{64}$ || "$listed_file" != "$package_name" ]]; then
    echo 'Package checksum must contain exactly the package filename and one SHA-256 value' >&2
    exit 1
fi
[[ "$(wc -l < "$checksum_file")" -eq 1 ]] || { echo 'Package checksum must have one line' >&2; exit 1; }
(cd "$INPUT_DIR" && sha256sum --check "$(basename "$checksum_file")")

PACKAGE_NAME="$(dpkg-deb -f "$package" Package)"
PACKAGE_ARCH="$(dpkg-deb -f "$package" Architecture)"
PACKAGE_VERSION="$(dpkg-deb -f "$package" Version)"
PACKAGE_MAINTAINER="$(dpkg-deb -f "$package" Maintainer)"
[[ "$PACKAGE_NAME" == 'milena' ]] || { echo 'Package field must be milena' >&2; exit 1; }
[[ "$PACKAGE_ARCH" == 'amd64' ]] || { echo 'Only Debian/Ubuntu amd64 is supported by this repository' >&2; exit 1; }
[[ -n "$PACKAGE_VERSION" && "$PACKAGE_VERSION" != *[[:space:]]* ]] || { echo 'Package Version is missing or contains whitespace' >&2; exit 1; }
[[ "$package_name" == "milena_${PACKAGE_VERSION}_amd64.deb" ]] || { echo 'Package filename Version does not match Debian metadata' >&2; exit 1; }
[[ "$PACKAGE_MAINTAINER" == *'<'*'@'*'>'* ]] || { echo 'Package Maintainer must contain an email address' >&2; exit 1; }
[[ "$PACKAGE_MAINTAINER" != *'.invalid>'* ]] || { echo 'Refusing a package with a placeholder Maintainer email' >&2; exit 1; }
VERSION="${MILENA_VERSION:-$PACKAGE_VERSION}"
VERSION="${VERSION#v}"
[[ "$VERSION" == "$PACKAGE_VERSION" ]] || {
    echo "MILENA_VERSION ($VERSION) does not match package Version ($PACKAGE_VERSION)" >&2
    exit 1
}

contents="$(dpkg-deb --contents "$package")"
grep -Eq '[[:space:]]\./usr/bin/milena$' <<<"$contents" || {
    echo 'Debian package does not install /usr/bin/milena' >&2
    exit 1
}
if grep -Eq '[[:space:]]\./data/data/com\.termux/' <<<"$contents"; then
    echo 'Termux/Bionic package paths cannot be published in the Debian/Ubuntu repository' >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR/pool/main/m/milena" \
    "$OUTPUT_DIR/dists/stable/main/binary-amd64"
cp "$package" "$OUTPUT_DIR/pool/main/m/milena/"
(
    cd "$OUTPUT_DIR"
    dpkg-scanpackages -a amd64 pool /dev/null > dists/stable/main/binary-amd64/Packages
)
gzip -9n < "$OUTPUT_DIR/dists/stable/main/binary-amd64/Packages" \
    > "$OUTPUT_DIR/dists/stable/main/binary-amd64/Packages.gz"

package_count="$(grep -c '^Package: milena$' "$OUTPUT_DIR/dists/stable/main/binary-amd64/Packages")"
[[ "$package_count" == '1' ]] || { echo "Expected one Packages stanza, found $package_count" >&2; exit 1; }
grep -qx 'Architecture: amd64' "$OUTPUT_DIR/dists/stable/main/binary-amd64/Packages" || {
    echo 'APT Packages index does not declare amd64' >&2
    exit 1
}
grep -qx "Version: $PACKAGE_VERSION" "$OUTPUT_DIR/dists/stable/main/binary-amd64/Packages" || {
    echo 'APT Packages index Version does not match the .deb' >&2
    exit 1
}

release_config="$OUTPUT_DIR/.apt-ftparchive.conf"
cat > "$release_config" <<'EOF_CONFIG'
APT::FTPArchive::Release::Origin "Milena";
APT::FTPArchive::Release::Label "Milena APT";
APT::FTPArchive::Release::Suite "stable";
APT::FTPArchive::Release::Codename "stable";
APT::FTPArchive::Release::Components "main";
APT::FTPArchive::Release::Architectures "amd64";
EOF_CONFIG
apt-ftparchive -c="$release_config" release "$OUTPUT_DIR/dists/stable" \
    > "$OUTPUT_DIR/dists/stable/Release"
rm -f "$release_config"
grep -qx 'Architectures: amd64' "$OUTPUT_DIR/dists/stable/Release" || {
    echo 'APT Release must declare exactly amd64' >&2
    exit 1
}

keyring="$OUTPUT_DIR/milena-archive-keyring.gpg"
gpg --batch --export "$KEY_ID" > "$keyring"
[[ -s "$keyring" ]] || { echo 'Could not export the APT public keyring' >&2; exit 1; }
gpg_args=(--batch --yes --local-user "$KEY_ID")
passphrase_file=''
cleanup() {
    if [[ -n "$passphrase_file" && -f "$passphrase_file" ]]; then
        rm -f "$passphrase_file"
    fi
}
trap cleanup EXIT
if [[ -n "${MILENA_GPG_PASSPHRASE:-}" ]]; then
    umask 077
    passphrase_file="$(mktemp)"
    printf '%s' "$MILENA_GPG_PASSPHRASE" > "$passphrase_file"
    gpg_args+=(--pinentry-mode loopback --passphrase-file "$passphrase_file")
fi
gpg "${gpg_args[@]}" --clearsign \
    --output "$OUTPUT_DIR/dists/stable/InRelease" "$OUTPUT_DIR/dists/stable/Release"
gpg "${gpg_args[@]}" --armor --detach-sign \
    --output "$OUTPUT_DIR/dists/stable/Release.gpg" "$OUTPUT_DIR/dists/stable/Release"
gpgv --keyring "$keyring" "$OUTPUT_DIR/dists/stable/InRelease"
gpgv --keyring "$keyring" "$OUTPUT_DIR/dists/stable/Release.gpg" \
    "$OUTPUT_DIR/dists/stable/Release"

source_commit="${GITHUB_SHA:-unknown}"
if [[ "$source_commit" == 'unknown' ]] && command -v git >/dev/null 2>&1; then
    source_commit="$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || printf 'unknown')"
fi
package_hash="$(sha256sum "$package" | awk '{print $1}')"
cat > "$OUTPUT_DIR/repository-provenance.txt" <<EOF_PROVENANCE
schema=milena-debian-apt-provenance-v1
package=$package_name
version=$PACKAGE_VERSION
architecture=amd64
sha256=$package_hash
source_commit=$source_commit
EOF_PROVENANCE
(
    cd "$OUTPUT_DIR"
    sha256sum \
        "pool/main/m/milena/$package_name" \
        dists/stable/main/binary-amd64/Packages \
        dists/stable/main/binary-amd64/Packages.gz \
        dists/stable/Release \
        dists/stable/InRelease \
        dists/stable/Release.gpg \
        milena-archive-keyring.gpg \
        repository-provenance.txt > SHA256SUMS
    sha256sum --check SHA256SUMS
)
printf 'Signed Debian/Ubuntu amd64 APT repository generated at %s\n' "$OUTPUT_DIR"
