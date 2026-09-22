#!/usr/bin/env python3
"""Static guardrails for the Termux packaging boundary."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ACTIVE = [
    ROOT / ".github/workflows/publish-apt.yml",
    ROOT / ".github/workflows/termux-aarch64-contract.yml",
    ROOT / "Makefile",
    ROOT / "packaging/termux/build-local-deb.sh",
    ROOT / "packaging/termux/generate-apt-repo.sh",
    ROOT / "packaging/termux/README.md",
    ROOT / "packaging/termux-packages/README.md",
    ROOT / "packaging/termux-packages/milena/build.sh",
    ROOT / "scripts/termux-install-smoke.sh",
    ROOT / "scripts/termux-real-smoke.sh",
    ROOT / "scripts/validate_termux_artifact.py",
    ROOT / "scripts/validate_termux_recipe.py",
    ROOT / "scripts/test_termux_packaging.py",
]
errors = []
forbidden_name = "".join(map(chr, (109, 97, 110, 111)))
for path in ACTIVE:
    if not path.is_file():
        errors.append(f"missing active packaging file: {path.relative_to(ROOT)}")
        continue
    if forbidden_name in path.name.lower() or forbidden_name in path.read_text(encoding="utf-8").lower():
        errors.append(f"unrelated historical package residue in {path.relative_to(ROOT)}")

recipe = ROOT / "packaging/termux-packages/milena/build.sh"
if recipe.is_file():
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts/validate_termux_recipe.py"), str(recipe)],
        cwd=ROOT, text=True, capture_output=True,
    )
    if result.returncode:
        errors.extend(line.removeprefix("ERROR: ") for line in result.stderr.splitlines() if line)

makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
recipe_text = recipe.read_text(encoding="utf-8") if recipe.is_file() else ""
common_text = (ROOT / "include/common.h").read_text(encoding="utf-8")
recipe_version = re.search(r"(?m)^TERMUX_PKG_VERSION=([^\s#]+)", recipe_text)
runtime_version = re.search(r"(?m)^#define MILENA_VERSION \"([^\"]+)\"", common_text)
if not recipe_version or not runtime_version or recipe_version.group(1) != runtime_version.group(1):
    errors.append("Termux package version must match MILENA_VERSION exactly")
if "TERMUX=1" not in makefile or "TERMUX_PREFIX" not in makefile:
    errors.append("Makefile lacks explicit Termux build/install variables")
for token in ("src/compiler.c", "src/ir.c", "src/vm.c"):
    if token in makefile:
        errors.append(f"experimental source enters canonical Makefile: {token}")
for token in ("/usr/bin", "/usr/local", "apt-get", "__GLIBC__"):
    if token.lower() in makefile.lower():
        errors.append(f"Makefile contains Debian/glibc path or dependency: {token}")

workflow = (ROOT / ".github/workflows/termux-aarch64-contract.yml").read_text(encoding="utf-8")
for required in (
    "workflow_dispatch:", "confirm_device:", "if: inputs.confirm_device == true",
    "runs-on: [self-hosted, termux, aarch64, milena]", "TERMUX_PACKAGES_DIR",
    "build-package.sh", "-I -f milena", "termux-runner-preflight.sh",
    "validate_termux_artifact.py", "termux-real-smoke.sh", "upload-artifact@v4",
    "if-no-files-found: error",
):
    if required not in workflow:
        errors.append(f"workflow missing required contract: {required}")
device_workflow = workflow.split("  termux-aarch64-contract:", 1)[-1]
if "ubuntu-latest" in device_workflow or "windows-latest" in device_workflow:
    errors.append("Termux device contract cannot use a hosted generic runner")

builder = (ROOT / "packaging/termux/build-local-deb.sh").read_text(encoding="utf-8")
for required in ("validate_termux_elf.py", "README.md", "SOURCE_DATE_EPOCH", ".provenance.json", "TERMUX=1"):
    if required not in builder:
        errors.append(f"local Termux builder missing: {required}")
for token in ("cp -R examples", "tests/", "include/", "src/compiler.c", "src/ir.c", "src/vm.c"):
    if token in builder:
        errors.append(f"local Termux package builder contains forbidden payload/path: {token}")

for path in (ROOT / "README.md", ROOT / "packaging/README.md", ROOT / "packaging/termux/README.md"):
    if path.is_file() and "pkg install milena" in path.read_text(encoding="utf-8"):
        text = path.read_text(encoding="utf-8").lower()
        if "todavía no" not in text and "aún no" not in text and "no existe" not in text:
            errors.append(f"unqualified pkg install claim in {path.relative_to(ROOT)}")

if errors:
    print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
    raise SystemExit(1)
print("Termux packaging guardrails: OK")
