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
import json
import shutil
import subprocess
import sys
from pathlib import Path

PREFIX_RE = re.compile(r"data/data/[A-Za-z0-9._-]+/files/usr/")
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
    expected_filename = f"milena_{meta['Version']}_aarch64.deb"
    if package.name != expected_filename:
        fail(f"package filename does not match Version: {package.name!r} != {expected_filename!r}")
    if meta["Architecture"] != "aarch64":
        fail(f"Architecture must be aarch64, got {meta['Architecture']!r}")
    for dependency in re.split(r"[,|]", meta.get("Depends", "")):
        name = re.split(r"[ (]", dependency.strip(), maxsplit=1)[0]
        if name in FORBIDDEN_DEPENDENCIES:
            fail(f"Debian dependency is not valid for Termux: {name}")

    paths = package_paths(package)
    executable_paths = [path for path in paths if path.endswith("/bin/milena") and PREFIX_RE.match(path)]
    if not executable_paths:
        fail("missing executable under $PREFIX")
    prefix_roots = {PREFIX_RE.match(path).group(0) for path in executable_paths}
    for path in paths:
        if path.startswith("/") or ".." in Path(path).parts:
            fail(f"unsafe package path: {path}")
        if not any(path.startswith(root) or root.startswith(path.rstrip("/") + "/") for root in prefix_roots):
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


def validate_provenance(package: Path, provenance: Path, require: bool = False) -> None:
    if not provenance.is_file():
        if require:
            fail(f"provenance file does not exist: {provenance}")
        return
    try:
        data = json.loads(provenance.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"invalid provenance JSON: {exc}")
    if data.get("artifact", {}).get("filename") != package.name:
        fail("provenance filename does not match the package")
    actual = hashlib.sha256(package.read_bytes()).hexdigest()
    if data.get("artifact", {}).get("sha256") != actual:
        fail("provenance SHA-256 does not match the package")
    if data.get("artifact", {}).get("architecture") != "aarch64":
        fail("provenance does not assert aarch64")
    if not data.get("source", {}).get("commit"):
        fail("provenance does not contain a source commit")


def validate_apt(root: Path, package: Path, keyring: Path | None,
                expected_fingerprint: str | None = None) -> None:
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
    debs = list((root / "pool").rglob("*.deb")) if (root / "pool").exists() else []
    if [p.relative_to(root) for p in debs] != [relative]:
        fail("APT pool must contain exactly the validated aarch64 package")
    binary_dirs = list((root / "dists").rglob("binary-*")) if (root / "dists").exists() else []
    if any(p.name != "binary-aarch64" for p in binary_dirs):
        fail("APT repository contains an undeclared architecture")
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
    if keyring:
        if not keyring.is_file() or keyring.stat().st_size == 0:
            fail(f"keyring does not exist or is empty: {keyring}")
        if not shutil.which("gpgv") or not shutil.which("gpg"):
            fail("gpg and gpgv are required when --keyring is provided")
        key_listing = run("gpg", "--batch", "--with-colons", "--show-keys", str(keyring))
        fingerprints = re.findall(r"^fpr:::::::::([0-9A-F]+):", key_listing, re.MULTILINE)
        if not fingerprints:
            fail("keyring contains no inspectable public key")
        if expected_fingerprint:
            wanted = re.sub(r"[^0-9A-Fa-f]", "", expected_fingerprint).upper()
            if wanted not in (value.upper() for value in fingerprints):
                fail("keyring fingerprint does not match the expected signing key")
        run("gpgv", "--keyring", str(keyring), str(release_gpg), str(release))
        run("gpgv", "--keyring", str(keyring), str(inrelease))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path)
    parser.add_argument("--checksum", type=Path)
    parser.add_argument("--apt-root", type=Path)
    parser.add_argument("--keyring", type=Path)
    parser.add_argument("--expected-fingerprint")
    parser.add_argument("--provenance", type=Path)
    parser.add_argument("--require-provenance", action="store_true")
    args = parser.parse_args()
    try:
        validate_package(args.package)
        if args.provenance:
            validate_provenance(args.package, args.provenance, args.require_provenance)
        elif args.require_provenance:
            fail("--require-provenance requires --provenance")
        if args.checksum:
            validate_checksum(args.package, args.checksum)
        if args.keyring and not args.apt_root:
            fail("--keyring requires --apt-root")
        if args.apt_root:
            validate_apt(args.apt_root, args.package, args.keyring, args.expected_fingerprint)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"Termux artifact boundary: OK ({args.package.name})")
    if args.apt_root:
        print(f"APT repository structure: OK ({args.apt_root})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
