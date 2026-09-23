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
    errors.append("missing manual Termux contract workflow")
else:
    workflow = WORKFLOW.read_text(encoding="utf-8")
    required = (
        "workflow_dispatch:", "confirm_device:", "if: inputs.confirm_device == true",
        "runs-on: [self-hosted, termux, aarch64, milena]", "TERMUX_PACKAGES_DIR",
        "build-package.sh", "-I -f milena", "termux-runner-preflight.sh",
        "validate_termux_recipe.py", "validate_termux_artifact.py",
        "validate_termux_elf.py", "termux-real-smoke.sh", "pkg install",
        "pkg upgrade", "pkg remove", "upload-artifact@v4", "if-no-files-found: error",
    )
    for fragment in required:
        if fragment not in workflow:
            errors.append(f"workflow missing required contract: {fragment}")
    device_workflow = workflow.split("  termux-aarch64-contract:", 1)[-1]
    if "ubuntu-latest" in device_workflow or "windows-latest" in device_workflow:
        errors.append("Termux device contract cannot use a hosted generic runner")

if not PREFLIGHT.is_file():
    errors.append("missing Termux runner preflight")
else:
    preflight = PREFLIGHT.read_text(encoding="utf-8")
    for fragment in (
        '[[ "$ARCH" == aarch64 ]]', '[[ "$DPKG_ARCH" == aarch64 ]]',
        '[[ "$PREFIX_DIR" == */usr', 'TERMUX_PACKAGES_DIR', 'termux-info', 'preflight.txt',
    ):
        if fragment not in preflight:
            errors.append(f"preflight missing required check: {fragment}")

plan = ROOT / "docs/TERMUX_VALIDATION_PLAN.md"
if not plan.is_file():
    errors.append("missing Termux validation documentation")
else:
    text = plan.read_text(encoding="utf-8")
    for label in ("self-hosted", "termux", "aarch64", "milena", "build-package.sh"):
        if label not in text:
            errors.append(f"documentation missing runner label/command: {label}")
    if not re.search(r"no crea ni registra un\s+runner|no se registró.*hardware", text, re.IGNORECASE):
        errors.append("documentation must state that no hardware was registered")

if errors:
    print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
    raise SystemExit(1)
print("Termux aarch64 runner contract: OK (manual, official build-package, non-emulated)")
