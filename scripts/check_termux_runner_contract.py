#!/usr/bin/env python3
"""Static checks for the manual, non-emulated Termux runner contract."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github/workflows/termux-aarch64-contract.yml"
PREFLIGHT = ROOT / "scripts/termux-runner-preflight.sh"
errors = []

if not WORKFLOW.is_file():
    errors.append("falta el workflow manual de contrato Termux")
else:
    workflow = WORKFLOW.read_text(encoding="utf-8")
    required = (
        "workflow_dispatch:",
        "confirm_device:",
        "if: inputs.confirm_device == true",
        "runs-on: [self-hosted, termux, aarch64, milena]",
        "termux-runner-preflight.sh",
        "build-local-deb.sh",
        "validate_termux_artifact.py",
        "if-no-files-found: error",
        "concurrency:",
        "cancel-in-progress: false",
        "termux-install-smoke.sh",
        "toolchain.txt",
    )
    for fragment in required:
        if fragment not in workflow:
            errors.append(f"workflow sin contrato requerido: {fragment}")
    if "ubuntu-latest" in workflow or "windows-latest" in workflow:
        errors.append("el contrato Termux no puede usar un runner hospedado genérico")

if not PREFLIGHT.is_file():
    errors.append("falta el preflight del runner Termux")
else:
    preflight = PREFLIGHT.read_text(encoding="utf-8")
    for fragment in (
        '[[ "$ARCH" == aarch64 ]]',
        '[[ "$DPKG_ARCH" == aarch64 ]]',
        'PREFIX_DIR" =~ ^/data/data/',
        'readelf',
        'workspace-sha256.txt',
        "termux-info",
        "preflight.txt",
    ):
        if fragment not in preflight:
            errors.append(f"preflight sin comprobación requerida: {fragment}")

plan = ROOT / "docs/TERMUX_VALIDATION_PLAN.md"
if not plan.is_file():
    errors.append("falta la documentación del contrato Termux")
else:
    text = plan.read_text(encoding="utf-8")
    for label in ("self-hosted", "termux", "aarch64", "milena"):
        if label not in text:
            errors.append(f"documentación sin etiqueta de runner: {label}")
    if not re.search(r"no crea ni registra un\s+runner|no se registró.*hardware", text, re.IGNORECASE):
        errors.append("la documentación debe declarar que no se registró hardware")

if errors:
    print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
    raise SystemExit(1)
print("Termux aarch64 runner contract: OK (manual, labelled, non-emulated)")
