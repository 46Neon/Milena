#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

PACKAGE="${1:?usage: termux-install-smoke.sh /path/to/milena_..._aarch64.deb}"
[[ -f "$PACKAGE" ]] || { echo "package missing: $PACKAGE" >&2; exit 1; }
command -v pkg >/dev/null || { echo 'pkg is required; this is not a Debian smoke test' >&2; exit 1; }
[[ "$(dpkg-deb -f "$PACKAGE" Package)" == milena ]] || { echo 'unexpected package name' >&2; exit 1; }
[[ "$(dpkg-deb -f "$PACKAGE" Architecture)" == aarch64 ]] || { echo 'unexpected package architecture' >&2; exit 1; }
if dpkg-query -W -f='${Status}' milena 2>/dev/null | grep -q 'install ok installed'; then
    echo 'refusing smoke test: milena is already installed; isolation is required' >&2
    exit 1
fi
cleanup() { pkg remove -y milena >/dev/null 2>&1 || true; }
trap cleanup EXIT

# Use only Milena's package lifecycle. Never upgrade unrelated packages on the device.
pkg install -y "$PACKAGE"
command -v milena >/dev/null
expected_version="$(dpkg-deb -f "$PACKAGE" Version)"
[[ "$(milena --version)" == "$expected_version" ]] || { echo 'installed binary version does not match package' >&2; exit 1; }
milena --help >/dev/null
milena --self-check
pkg remove -y milena
! dpkg-query -W -f='${Status}' milena 2>/dev/null | grep -q 'install ok installed'
printf '%s\n' 'Termux pkg install/remove smoke: OK'
