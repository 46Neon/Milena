#!/usr/bin/env python3
"""Static guardrails for the Milena Termux/APT publishing boundary."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

# These are active packaging inputs. Historical Git objects are intentionally not
# scanned: a closed PR is history, not a runnable release definition.
ACTIVE = [
    ROOT / '.github/workflows/publish-apt.yml',
    ROOT / 'packaging/termux/build-local-deb.sh',
    ROOT / 'packaging/termux/generate-apt-repo.sh',
    ROOT / 'packaging/README.md',
    ROOT / 'packaging/termux/README.md',
    ROOT / 'packaging/apt/README.md',
    ROOT / 'PLAN_MILENA.md',
    ROOT / 'README.md',
    ROOT / 'scripts/validate_termux_artifact.py',
    ROOT / 'scripts/test_termux_packaging.py',
    ROOT / 'scripts/check_compiler_boundary.py',
    ROOT / 'docs/TERMUX_VALIDATION_PLAN.md',
]
errors = []
legacy_name = 'Ma' + 'no'
legacy_upper = legacy_name.upper()
for path in ACTIVE:
    if not path.is_file():
        errors.append(f'falta archivo activo: {path.relative_to(ROOT)}')
        continue
    text = path.read_text(encoding='utf-8')
    if legacy_name.lower() in text.lower():
        errors.append(f'referencia activa a {legacy_name}: {path.relative_to(ROOT)}')

workflow = (ROOT / '.github/workflows/publish-apt.yml').read_text(encoding='utf-8')
generator = (ROOT / 'packaging/termux/generate-apt-repo.sh').read_text(encoding='utf-8')
builder = (ROOT / 'packaging/termux/build-local-deb.sh').read_text(encoding='utf-8')

if (ROOT / 'packaging/ci/publish-apt.yml').exists():
    errors.append('hay una segunda definición ejecutable de publicación APT')
for forbidden in ("--pattern '*.deb'", legacy_upper + '_', legacy_name.lower() + '-archive-keyring', 'Architecture: amd64'):
    if forbidden in workflow:
        errors.append(f'workflow de publicación contiene {forbidden!r}')
for required in ('MILENA_GPG_PRIVATE_KEY', 'MILENA_GPG_KEY_ID',
                 'MILENA_GPG_PASSPHRASE', "--pattern 'milena_*_aarch64.deb'",
                 'binary-aarch64/Packages.gz',
                 'scripts/validate_termux_artifact.py',
                 '--apt-root dist/apt'):
    if required not in workflow:
        errors.append(f'workflow sin control requerido: {required}')
for required in ('MILENA_GPG_KEY_ID', 'MILENA_GPG_PASSPHRASE',
                 'Architectures "aarch64"', 'binary-aarch64',
                 'PACKAGE_ARCH" == aarch64'):
    if required not in generator:
        errors.append(f'generador APT sin control requerido: {required}')
if 'MILENA_GPG_' in builder or 'GPG_' in builder:
    errors.append('el constructor local no debe manejar claves de firma')
if "[[ \"$ARCH\" == 'aarch64' ]]" not in builder:
    errors.append('el constructor Termux no restringe la arquitectura aarch64')

# Keep the public wording honest: no current claim may turn a planned command
# into an advertised installation path.
for path in (ROOT / 'README.md', ROOT / 'packaging/README.md', ROOT / 'packaging/termux/README.md'):
    text = path.read_text(encoding='utf-8')
    if 'pkg install milena' in text:
        # The explicit checks below are less locale-sensitive than the guard's
        # optional regex; this branch catches an unqualified command in docs.
        if 'todavía no' not in text.lower() and 'aún no' not in text.lower():
            errors.append(f'comando pkg install sin estado no-oficial en {path.relative_to(ROOT)}')

if errors:
    print('\n'.join(f'ERROR: {error}' for error in errors), file=sys.stderr)
    raise SystemExit(1)
print('Termux/APT packaging guardrails: OK')

