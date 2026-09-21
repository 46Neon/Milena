#!/usr/bin/env bash
# Reproducible host-side contract for the canonical Termux binary.
# This validates the source/CLI boundary only; it never emulates Android/Bionic.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

make clean >/dev/null
make TERMUX=1 CC="${CC:-clang}" all

version=$(./milena --version)
test -n "$version"
./milena --help | grep -Fq 'Uso:'
./milena --self-check | grep -Fq 'Milena self-check: OK'
if ./milena --no-existe >/dev/null 2>&1; then
    echo 'ERROR: una opción desconocida terminó con éxito' >&2
    exit 1
fi

# The host result is useful for C17/link/source checks, but is not an Android claim.
if command -v readelf >/dev/null 2>&1; then
    readelf -h ./milena | grep -Fq 'ELF'
fi
printf 'Contrato Termux canónico: OK (%s; validación host, no Android emulado)\n' "$version"
