#!/usr/bin/env python3
"""Fail-closed static gate for the Termux release boundary."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []
def require(path: str, *fragments: str) -> None:
    file = ROOT / path
    if not file.is_file():
        errors.append(f'missing required file: {path}')
        return
    text = file.read_text(encoding='utf-8')
    for fragment in fragments:
        if fragment not in text:
            errors.append(f'{path} missing required contract: {fragment}')

require('.github/workflows/termux-aarch64-contract.yml',
        'concurrency:', 'cancel-in-progress: false', 'confirm_device:',
        'if: inputs.confirm_device == true', 'timeout-minutes:',
        'termux-install-smoke.sh', 'artifacts/termux-runner/toolchain.txt',
        'if-no-files-found: error')
require('.github/workflows/publish-apt.yml',
        'concurrency:', 'confirm_publish:', 'if: inputs.confirm_publish == true',
        "--pattern 'milena_*_aarch64.deb'", 'gpgv',
        '--provenance', '--require-provenance', '--require-elf', 'if-no-files-found: error')
require('scripts/termux-runner-preflight.sh',
        'uname -m', 'dpkg --print-architecture', 'termux-info',
        'readelf', 'workspace-sha256.txt')
require('scripts/validate_termux_artifact.py',
        'expected_filename', 'provenance', 'gpgv', 'binary-aarch64')
require('packaging/termux/generate-apt-repo.sh',
        'PACKAGE_ARCH', 'sha256sum', 'MILENA_GPG_KEY_ID', 'binary-aarch64')
require('docs/TERMUX_VALIDATION_PLAN.md', 'no crea ni registra',
        'install', 'actualización', 'eliminación', 'SBOM')
require('docs/TERMUX_SECURITY.md', 'Trust boundary', 'gpgv')
require('docs/TERMUX_RELEASE_CHECKLIST.md', 'confirm_device=true', 'confirm_publish=true')
require('docs/TERMUX_INCIDENT_RUNBOOK.md', 'Artefacto o firma inválida', 'Runner no conforme')
workflow = (ROOT / '.github/workflows/publish-apt.yml').read_text(encoding='utf-8')
if '\n  push:' in workflow:
    errors.append('APT publication must be manual; tag pushes are not an approval')
contract = (ROOT / '.github/workflows/termux-aarch64-contract.yml').read_text(encoding='utf-8')
if 'ubuntu-latest' in contract or 'windows-latest' in contract:
    errors.append('Termux contract may not use a hosted runner')
if errors:
    print('\n'.join('ERROR: ' + e for e in errors), file=sys.stderr)
    raise SystemExit(1)
print('Termux industrial static contract: OK')
