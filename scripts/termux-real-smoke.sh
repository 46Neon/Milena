#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

PACKAGE="${1:?usage: termux-real-smoke.sh PACKAGE [APT_REPOSITORY_URL]}"
APT_REPOSITORY="${2:-${MILENA_APT_REPOSITORY:-}}"
REPORT="${TERMUX_SMOKE_REPORT:-artifacts/termux-runner/smoke.txt}"
mkdir -p "$(dirname "$REPORT")"
{
    printf 'package=%s\n' "$PACKAGE"
    printf 'uname_m=%s\n' "$(uname -m)"
    printf 'dpkg_architecture=%s\n' "$(dpkg --print-architecture)"
    printf 'prefix=%s\n' "${PREFIX:-}"
    printf 'local_install=begin\n'
} > "$REPORT"

# This is intentionally destructive only to the package named milena; the
# contract runner must be disposable or dedicated to this validation.
dpkg --install "$PACKAGE" >> "$REPORT" 2>&1
command -v milena >> "$REPORT"
milena --help >> "$REPORT" 2>&1 || true
dpkg-query -W -f='${Status}\n' milena >> "$REPORT"
dpkg --purge milena >> "$REPORT" 2>&1
printf 'local_install=passed\n' >> "$REPORT"

if [[ -z "$APT_REPOSITORY" ]]; then
    printf 'apt_smoke=not-run (no repository URL was supplied; fail-closed)\n' >> "$REPORT"
    echo 'Local Termux package smoke passed; APT smoke was not run because no repository URL was supplied.'
    exit 0
fi
if [[ "$APT_REPOSITORY" != https://* ]]; then
    echo 'APT repository URL must be HTTPS; refusing to alter Termux sources.' >&2
    exit 1
fi
# The operator must preinstall the archive keyring and configure this source.
# This script never invents a host, key, source-list entry or secret.
pkg update -y >> "$REPORT" 2>&1
pkg install -y milena >> "$REPORT" 2>&1
command -v milena >> "$REPORT"
milena --help >> "$REPORT" 2>&1 || true
pkg uninstall -y milena >> "$REPORT" 2>&1
printf 'apt_smoke=passed\napt_repository=%s\n' "$APT_REPOSITORY" >> "$REPORT"
echo 'Local and configured APT Termux package smoke passed.'
