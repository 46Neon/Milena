#!/usr/bin/env bash
set -euo pipefail

# Fail-closed identity check. Labels select a host; they never prove it is Termux.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="$(uname -m)"
DPKG_ARCH="$(dpkg --print-architecture)"
PREFIX_DIR="${PREFIX:-}"
RUNNER_ARCH_VALUE="${RUNNER_ARCH:-unknown}"
RUNNER_NAME_VALUE="${RUNNER_NAME:-unknown}"
RUNNER_OS_VALUE="${RUNNER_OS:-unknown}"

[[ "$ARCH" == aarch64 ]] || { echo "uname -m must be aarch64 (got $ARCH)" >&2; exit 1; }
[[ "$DPKG_ARCH" == aarch64 ]] || { echo "dpkg architecture must be aarch64 (got $DPKG_ARCH)" >&2; exit 1; }
[[ "$PREFIX_DIR" =~ ^/data/data/[A-Za-z0-9._-]+/files/usr$ ]] || {
    echo 'PREFIX must be the real Termux .../files/usr directory' >&2
    exit 1
}
[[ -d "$PREFIX_DIR" && -x "$PREFIX_DIR/bin" ]] || { echo 'PREFIX is not usable' >&2; exit 1; }
[[ "$ROOT_DIR" != /data/data/*/files/usr/* ]] || { echo 'workspace must not be inside PREFIX' >&2; exit 1; }
[[ -z "$(git -C "$ROOT_DIR" status --porcelain --untracked-files=all)" ]] || {
    echo 'workspace is not clean before the build' >&2
    exit 1
}

for command in clang make dpkg dpkg-deb python3 sha256sum readelf pkg termux-info getprop; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing required Termux command: $command" >&2
        exit 1
    }
done
TERMUX_INFO="$(termux-info 2>&1)"
printf '%s\n' "$TERMUX_INFO" | grep -qi termux || {
    echo 'termux-info did not identify a Termux host' >&2
    exit 1
}
if [[ -f /etc/os-release ]] && grep -Eiq '^(ID|ID_LIKE)=.*(debian|ubuntu)' /etc/os-release; then
    echo 'Debian/Ubuntu host is not a Termux runner' >&2
    exit 1
fi
TARGET_TRIPLE="$(clang -print-target-triple 2>/dev/null || true)"
printf '%s' "$TARGET_TRIPLE" | grep -Eqi 'aarch64' || {
    echo "Clang target is not aarch64: $TARGET_TRIPLE" >&2
    exit 1
}
printf '%s' "$TARGET_TRIPLE" | grep -Eqi 'android' || {
    echo "Clang target is not Android/bionic: $TARGET_TRIPLE" >&2
    exit 1
}
ANDROID_API="$(getprop ro.build.version.sdk 2>/dev/null || true)"
[[ "$ANDROID_API" =~ ^[0-9]+$ ]] || {
    echo "Android SDK property is unavailable; this is not a verifiable device runner" >&2
    exit 1
}

EVIDENCE_DIR="$ROOT_DIR/artifacts/termux-runner"
rm -rf "$EVIDENCE_DIR"
mkdir -p "$EVIDENCE_DIR"
{
    printf 'runner_contract=registered-self-hosted-termux-aarch64\n'
    printf 'runner_name=%s\n' "$RUNNER_NAME_VALUE"
    printf 'runner_os=%s\n' "$RUNNER_OS_VALUE"
    printf 'runner_arch=%s\n' "$RUNNER_ARCH_VALUE"
    printf 'uname_m=%s\n' "$ARCH"
    printf 'dpkg_architecture=%s\n' "$DPKG_ARCH"
    printf 'prefix=%s\n' "$PREFIX_DIR"
    printf 'target_triple=%s\n' "$TARGET_TRIPLE"
    printf 'android_api=%s\n' "$ANDROID_API"
    printf 'commit=%s\n' "$(git -C "$ROOT_DIR" rev-parse HEAD)"
    printf 'termux_info<<EOF\n%s\nEOF\n' "$TERMUX_INFO"
} > "$EVIDENCE_DIR/preflight.txt"
{
    printf '%s\n' '== clang ==' ; clang --version
    printf '%s\n' '== make ==' ; make --version | head -n 2
    printf '%s\n' '== dpkg ==' ; dpkg --version | head -n 2
    printf '%s\n' '== python ==' ; python3 --version
    printf '%s\n' '== pkg ==' ; pkg --version
    printf '%s\n' '== target ==' ; printf '%s\n' "$TARGET_TRIPLE"
    printf '%s\n' '== android-api ==' ; printf '%s\n' "$ANDROID_API"
} > "$EVIDENCE_DIR/toolchain.txt"
# This is a record, not an assertion that Android execution occurred elsewhere.
find "$ROOT_DIR" -maxdepth 2 -type f -print0 | sort -z | xargs -0 sha256sum > "$EVIDENCE_DIR/workspace-sha256.txt"
printf 'Termux runner preflight: OK (aarch64, PREFIX=%s)\n' "$PREFIX_DIR"
