#!/usr/bin/env python3
"""Validate the candidate recipe without pretending it is in Termux repos."""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
import urllib.request
from pathlib import Path

REQUIRED = (
    "TERMUX_PKG_HOMEPAGE", "TERMUX_PKG_DESCRIPTION", "TERMUX_PKG_LICENSE",
    "TERMUX_PKG_MAINTAINER", "TERMUX_PKG_VERSION", "TERMUX_PKG_SRCURL",
    "TERMUX_PKG_SHA256", "TERMUX_PKG_DEPENDS", "TERMUX_PKG_BUILD_IN_SRC",
)
FORBIDDEN = ("@REPLACE", "/usr/local", "apt-get", "dpkg-buildpackage", "glibc")


def assignment(text: str, name: str) -> str | None:
    m = re.search(rf"(?m)^\s*{re.escape(name)}=(?:\"([^\"]*)\"|'([^']*)'|([^\s#]+))", text)
    if not m:
        return None
    return next((x for x in m.groups() if x is not None), "")


def validate(recipe: Path, fetch: bool = False, official_dir: Path | None = None) -> list[str]:
    errors: list[str] = []
    text = recipe.read_text(encoding="utf-8")
    values = {name: assignment(text, name) for name in REQUIRED}
    for name, value in values.items():
        if value is None:
            errors.append(f"missing {name}")
    if errors:
        return errors
    if values["TERMUX_PKG_SHA256"] is None or not re.fullmatch(r"[0-9a-fA-F]{64}", values["TERMUX_PKG_SHA256"]):
        errors.append("TERMUX_PKG_SHA256 must be a verified 64-hex digest, never a placeholder")
    version = values["TERMUX_PKG_VERSION"] or ""
    srcurl = values["TERMUX_PKG_SRCURL"] or ""
    if not re.fullmatch(r"[0-9][0-9A-Za-z.+:~-]*", version):
        errors.append("TERMUX_PKG_VERSION is not a valid package version")
    if "${TERMUX_PKG_VERSION}" not in srcurl or "/archive/refs/tags/v" not in srcurl:
        errors.append("TERMUX_PKG_SRCURL must be a versioned upstream tag using TERMUX_PKG_VERSION")
    if values["TERMUX_PKG_BUILD_IN_SRC"] != "true":
        errors.append("TERMUX_PKG_BUILD_IN_SRC must be true for this source tree")
    if "termux_step_make()" not in text or "termux_step_make_install()" not in text:
        errors.append("recipe must define official termux build/install steps")
    if "$TERMUX_PREFIX/bin/milena" not in text:
        errors.append("install path must use TERMUX_PREFIX, not a Debian prefix")
    for token in FORBIDDEN:
        if token.lower() in text.lower():
            errors.append(f"recipe contains forbidden non-Termux token: {token}")
    # The Termux bash shebang legitimately contains .../com.termux/files/usr/bin;
    # reject only an unqualified Debian /usr/bin destination.
    if re.search(r"(?<!com\.termux/files)/usr/bin(?:[/'\"]|$)", text):
        errors.append("recipe contains a Debian /usr/bin path")
    if fetch and not errors:
        url = srcurl.replace("${TERMUX_PKG_VERSION}", version)
        try:
            with urllib.request.urlopen(url, timeout=30) as response:
                digest = hashlib.sha256(response.read()).hexdigest()
        except Exception as exc:  # pragma: no cover - network is optional
            errors.append(f"could not fetch TERMUX_PKG_SRCURL for digest validation: {exc}")
        else:
            if digest.lower() != (values["TERMUX_PKG_SHA256"] or "").lower():
                errors.append(f"source SHA256 mismatch: fetched {digest}")
    if official_dir is not None:
        build = official_dir / "build-package.sh"
        if not build.is_file():
            errors.append(f"official checkout missing {build}")
        elif not build.stat().st_mode & 0o111:
            errors.append(f"official build-package.sh is not executable: {build}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("recipe", nargs="?", default="packaging/termux-packages/milena/build.sh")
    parser.add_argument("--fetch", action="store_true", help="download SRCURL and verify SHA256")
    parser.add_argument("--official-dir", type=Path, help="validate an existing termux-packages checkout")
    args = parser.parse_args()
    errors = validate(Path(args.recipe), args.fetch, args.official_dir)
    if errors:
        print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
        return 1
    print("Termux candidate recipe: valid metadata and Termux paths")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
