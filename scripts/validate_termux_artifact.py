#!/usr/bin/env python3
"""Validate the Termux package boundary without pretending to run Termux.

This validates Debian metadata, the Termux prefix layout, checksums, and (when
requested) the generated APT index. It does not build a Termux binary or prove
that the package installs on Android.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

PREFIX = "data/data/com.termux/files/usr/"
FORBIDDEN_DEPENDENCIES = {"libc6", "libgcc-s1", "libstdc++6", "linux-libc-dev"}


def fail(message: str) -> None:
    raise ValueError(message)


def run(*args: str) -> str:
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as exc:
        output = getattr(exc, "output", "")
        fail(f"{args[0]} failed: {output.strip()}")


def fields(package: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for field in ("Package", "Version", "Architecture", "Depends"):
        result[field] = run("dpkg-deb", "-f", str(package), field).strip()
    return result


def package_paths(package: Path) -> list[str]:
    lines = run("dpkg-deb", "--contents", str(package)).splitlines()
    paths: list[str] = []
    for line in lines:
        parts = line.split(maxsplit=5)
        if len(parts) < 6:
            fail(f"unparseable package entry: {line}")
        path = parts[5].removeprefix("./")
        if path in ("", "."):
            continue
        paths.append(path)
    return paths


def validate_package(package: Path) -> dict[str, str]:
    if not package.is_file():
        fail(f"package does not exist: {package}")
    if not re.fullmatch(r"milena_[^/]+_aarch64\.deb", package.name):
        fail(f"package filename is not the Termux aarch64 form: {package.name}")
    meta = fields(package)
    if meta["Package"] != "milena":
        fail(f"Package must be milena, got {meta['Package']!r}")
    if not meta["Version"] or any(ch.isspace() for ch in meta["Version"]):
        fail("package Version is empty or contains whitespace")
    if meta["Architecture"] != "aarch64":
        fail(f"Architecture must be aarch64, got {meta['Architecture']!r}")
    for dependency in re.split(r"[,|]", meta.get("Depends", "")):
        name = re.split(r"[ (]", dependency.strip(), maxsplit=1)[0]
        if name in FORBIDDEN_DEPENDENCIES:
            fail(f"Debian dependency is not valid for Termux: {name}")

    paths = package_paths(package)
    if f"{PREFIX}bin/milena" not in paths:
        fail(f"missing executable under $PREFIX: {PREFIX}bin/milena")
    for path in paths:
        if path.startswith("/") or ".." in Path(path).parts:
            fail(f"unsafe package path: {path}")
        if not path.startswith(PREFIX) and not PREFIX.startswith(path.rstrip("/") + "/"):
            fail(f"package path escapes the Termux prefix: {path}")
        if path.startswith(("usr/", "bin/", "lib/", "etc/")):
            fail(f"Debian filesystem path in Termux package: {path}")
    return meta


def validate_checksum(package: Path, checksum: Path) -> None:
    if not checksum.is_file():
        fail(f"checksum file does not exist: {checksum}")
    lines = [line.strip() for line in checksum.read_text(encoding="utf-8").splitlines() if line.strip()]
    if len(lines) != 1:
        fail("checksum file must contain exactly one entry")
    match = re.fullmatch(r"([0-9a-fA-F]{64})\s+[* ]?(.+)", lines[0])
    if not match:
        fail("checksum file is not a SHA-256 checksum entry")
    expected, listed = match.groups()
    if Path(listed).name != package.name:
        fail(f"checksum refers to {listed!r}, not {package.name!r}")
    actual = hashlib.sha256(package.read_bytes()).hexdigest()
    if actual.lower() != expected.lower():
        fail("package SHA-256 does not match its checksum file")


def apt_stanza(text: str) -> dict[str, str]:
    stanza: dict[str, str] = {}
    for line in text.splitlines():
        if ": " in line:
            key, value = line.split(": ", 1)
            stanza[key] = value
    return stanza


def validate_apt(root: Path, package: Path, keyring: Path | None) -> None:
    relative = Path("pool/main/m/milena") / package.name
    copied = root / relative
    if not copied.is_file():
        fail(f"APT pool does not contain {relative}")
    if hashlib.sha256(copied.read_bytes()).digest() != hashlib.sha256(package.read_bytes()).digest():
        fail("APT pool package differs from the validated input package")
    packages_gz = root / "dists/stable/main/binary-aarch64/Packages.gz"
    packages_file = root / "dists/stable/main/binary-aarch64/Packages"
    release = root / "dists/stable/Release"
    inrelease = root / "dists/stable/InRelease"
    release_gpg = root / "dists/stable/Release.gpg"
    key_file = root / "milena-archive-keyring.asc"
    for required in (packages_gz, release, inrelease, release_gpg, key_file):
        if not required.is_file() or required.stat().st_size == 0:
            fail(f"missing or empty APT metadata: {required.relative_to(root)}")
    if packages_file.is_file():
        packages_text = packages_file.read_text(encoding="utf-8")
    else:
        try:
            packages_text = gzip.decompress(packages_gz.read_bytes()).decode()
        except Exception as exc:
            fail(f"cannot decompress Packages.gz: {exc}")
    stanzas = [apt_stanza(block) for block in packages_text.split("\n\n") if block.strip()]
    if len(stanzas) != 1:
        fail(f"expected one aarch64 Packages stanza, got {len(stanzas)}")
    stanza = stanzas[0]
    expected_hash = hashlib.sha256(copied.read_bytes()).hexdigest()
    for key, expected in (("Package", "milena"), ("Architecture", "aarch64"),
                          ("Filename", str(relative)), ("SHA256", expected_hash)):
        if stanza.get(key) != expected:
            fail(f"Packages.gz {key} mismatch: {stanza.get(key)!r} != {expected!r}")
    if stanza.get("Version") != run("dpkg-deb", "-f", str(package), "Version").strip():
        fail("Packages.gz Version does not match the package")
    release_text = release.read_text(encoding="utf-8")
    if not re.search(r"^Architectures:\s+aarch64\s*$", release_text, re.MULTILINE):
        fail("Release does not declare exactly the aarch64 architecture")
    if re.search(r"(?:amd64|arm64|armhf|i386|x86_64)", release_text, re.IGNORECASE):
        fail("Release declares an architecture other than aarch64")
    digest_line = re.search(r"^\s*([0-9a-fA-F]{64})\s+\d+\s+main/binary-aarch64/Packages\.gz\s*$", release_text, re.MULTILINE)
    if not digest_line or digest_line.group(1).lower() != hashlib.sha256(packages_gz.read_bytes()).hexdigest():
        fail("Release does not contain the correct Packages.gz SHA-256")
    for path in (root / "dists").rglob("binary-*"):
        if path.name != "binary-aarch64":
            fail(f"APT repository contains an undeclared architecture: {path}")
    if keyring:
        if not keyring.is_file():
            fail(f"keyring does not exist: {keyring}")
        if not shutil.which("gpgv"):
            fail("gpgv is required when --keyring is provided")
        run("gpgv", "--keyring", str(keyring), str(release_gpg), str(release))
        run("gpgv", "--keyring", str(keyring), str(inrelease))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path)
    parser.add_argument("--checksum", type=Path)
    parser.add_argument("--apt-root", type=Path)
    parser.add_argument("--keyring", type=Path)
    args = parser.parse_args()
    try:
        validate_package(args.package)
        if args.checksum:
            validate_checksum(args.package, args.checksum)
        if args.keyring and not args.apt_root:
            fail("--keyring requires --apt-root")
        if args.apt_root:
            validate_apt(args.apt_root, args.package, args.keyring)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"Termux artifact boundary: OK ({args.package.name})")
    if args.apt_root:
        print(f"APT repository structure: OK ({args.apt_root})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
