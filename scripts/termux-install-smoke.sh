#!/usr/bin/env bash
set -euo pipefail

PACKAGE="${1:?usage: termux-install-smoke.sh /path/to/milena_..._aarch64.deb}"
[[ -f "$PACKAGE" ]] || { echo "package missing: $PACKAGE" >&2; exit 1; }
[[ "$(dpkg-deb -f "$PACKAGE" Package)" == milena ]] || { echo 'unexpected package name' >&2; exit 1; }
[[ "$(dpkg-deb -f "$PACKAGE" Architecture)" == aarch64 ]] || { echo 'unexpected package architecture' >&2; exit 1; }
if dpkg-query -W -f='${Status}' milena 2>/dev/null | grep -q 'install ok installed'; then
    echo 'refusing smoke test: milena is already installed; isolation is required' >&2
    exit 1
fi
cleanup() { dpkg --purge milena >/dev/null 2>&1 || true; }
trap cleanup EXIT

dpkg --install "$PACKAGE"
command -v milena >/dev/null
milena --help >/dev/null 2>&1 || true
# A second install exercises the upgrade/reconfigure path without inventing a second artifact.
dpkg --install "$PACKAGE"
dpkg-query -W -f='${Status}' milena | grep -q 'install ok installed'
dpkg --purge milena
! dpkg-query -W -f='${Status}' milena 2>/dev/null | grep -q 'install ok installed'
printf '%s\n' 'Termux install/upgrade/remove smoke: OK'
