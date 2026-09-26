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
    printf 'lifecycle=official-termux-pkg-install-remove\n'
} > "$REPORT"

# This tests only Milena's package lifecycle; it never upgrades unrelated device packages.
./scripts/termux-install-smoke.sh "$PACKAGE" >> "$REPORT" 2>&1
printf 'local_pkg_lifecycle=passed\n' >> "$REPORT"

if [[ -z "$APT_REPOSITORY" ]]; then
    printf 'apt_smoke=not-run (no preconfigured HTTPS repository URL; fail-closed)\n' >> "$REPORT"
    echo 'Local Termux pkg lifecycle passed; configured APT smoke was not run.'
    exit 0
fi
if [[ "$APT_REPOSITORY" != https://* ]]; then
    echo 'APT repository URL must be HTTPS; refusing to alter Termux sources.' >&2
    exit 1
fi
# The operator must preinstall the archive keyring and configure this source.
pkg update -y >> "$REPORT" 2>&1
pkg install -y milena >> "$REPORT" 2>&1
command -v milena >> "$REPORT"
milena --help >> "$REPORT" 2>&1 || true
pkg remove -y milena >> "$REPORT" 2>&1
printf 'apt_smoke=passed (install/remove only)\napt_repository=%s\n' "$APT_REPOSITORY" >> "$REPORT"
echo 'Local and configured APT Termux package smoke passed (install/remove only).'
