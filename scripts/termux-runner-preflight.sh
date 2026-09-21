#!/usr/bin/env bash
set -euo pipefail

# This script refuses to turn a label into an architecture claim. It must run
# on the registered Termux host selected by the workflow labels.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="$(uname -m)"
DPKG_ARCH="$(dpkg --print-architecture)"
PREFIX_DIR="${PREFIX:-}"
[[ "$ARCH" == aarch64 ]] || { echo "uname -m must be aarch64 (got $ARCH)" >&2; exit 1; }
[[ "$DPKG_ARCH" == aarch64 ]] || { echo "dpkg architecture must be aarch64 (got $DPKG_ARCH)" >&2; exit 1; }
[[ "$PREFIX_DIR" == */usr && "$PREFIX_DIR" != *[[:space:]]* ]] || {
    echo 'PREFIX must point to the Termux .../usr directory' >&2
    exit 1
}
for command in clang make dpkg dpkg-deb python3 sha256sum readelf; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing required Termux command: $command" >&2
        exit 1
    }
done

EVIDENCE_DIR="$ROOT_DIR/artifacts/termux-runner"
mkdir -p "$EVIDENCE_DIR"
{
    printf 'runner=registered-self-hosted-termux-aarch64\n'
    printf 'uname_m=%s\n' "$ARCH"
    printf 'dpkg_architecture=%s\n' "$DPKG_ARCH"
    printf 'prefix=%s\n' "$PREFIX_DIR"
    printf 'termux_version='; (termux-info 2>/dev/null || true) | head -n 1
    printf 'clang='; clang --version | head -n 1
    printf 'commit='; git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || printf 'unknown'
} > "$EVIDENCE_DIR/preflight.txt"
{ clang --version; readelf --version 2>/dev/null | head -n 1; dpkg-deb --version | head -n 1; } > "$EVIDENCE_DIR/toolchain.txt"
( cd "$ROOT_DIR" && find . -type f -not -path './.git/*' -print0 | sort -z | xargs -0 sha256sum ) > "$EVIDENCE_DIR/workspace-sha256.txt"
printf 'Termux runner preflight: OK (aarch64, PREFIX=%s)\n' "$PREFIX_DIR"

