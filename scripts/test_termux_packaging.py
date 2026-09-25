#!/usr/bin/env python3
"""Reproducible boundary tests for the Termux/APT validator.

The fixture is deliberately a metadata-only package. It is not a Termux build
and this test must never be described as an install or execution test on
Android.
"""
from __future__ import annotations

import gzip
import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "scripts/validate_termux_artifact.py"
RECIPE_VALIDATOR = ROOT / "tools/check_repository_contracts"
RECIPE = ROOT / "packaging/termux-packages/milena/build.sh"


def command(*args: str) -> None:
    subprocess.run(args, check=True, cwd=ROOT)


def make_package(directory: Path, architecture: str = "aarch64", debian_path: bool = False) -> Path:
    stage = directory / "stage"
    target = stage / ("usr/bin" if debian_path else "data/data/com.termux/files/usr/bin")
    target.mkdir(parents=True)
    (target / "milena").write_bytes(b"fixture, not a compiled Termux binary\n")
    (target / "milena").chmod(0o755)
    doc = stage / "data/data/com.termux/files/usr/share/doc/milena"
    doc.mkdir(parents=True)
    (doc / "README.md").write_text("fixture documentation\n", encoding="utf-8")
    control = stage / "DEBIAN"
    control.mkdir()
    (control / "control").write_text(
        "Package: milena\nVersion: 0.2.0\n"
        f"Architecture: {architecture}\nMaintainer: Milena Fixture <fixture@example.invalid>\nDescription: fixture\n",
        encoding="utf-8",
    )
    package = directory / "milena_0.2.0_aarch64.deb"
    command("dpkg-deb", "--build", "--root-owner-group", str(stage), str(package))
    return package


def run_validator(package: Path, *extra: str, expect_success: bool = True) -> None:
    result = subprocess.run(
        [sys.executable, str(VALIDATOR), str(package), *extra],
        cwd=ROOT, text=True, capture_output=True,
    )
    if (result.returncode == 0) != expect_success:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise AssertionError(f"validator success={result.returncode == 0}, expected {expect_success}")


def run_recipe(path: Path, *extra: str, expect_success: bool = True) -> None:
    result = subprocess.run(
        [str(RECIPE_VALIDATOR), "termux-recipe", str(path), *extra],
        cwd=ROOT, text=True, capture_output=True,
    )
    if (result.returncode == 0) != expect_success:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise AssertionError(f"recipe validator success={result.returncode == 0}, expected {expect_success}")


def main() -> int:
    run_recipe(RECIPE)
    with tempfile.TemporaryDirectory(prefix="milena-termux-recipe-") as raw_recipe:
        work = Path(raw_recipe)
        base = RECIPE.read_text(encoding="utf-8")
        sha_line = "TERMUX_PKG_SHA256=56e189bbd1e89aa25a7e8588e0606f0ea42d3bf5f1086fcfa3442d632d571153"
        missing_sha = work / "missing-sha.sh"
        missing_sha.write_text(base.replace(sha_line, "TERMUX_PKG_SHA256=\n" + sha_line), encoding="utf-8")
        run_recipe(missing_sha)
        unterminated = work / "unterminated-then-valid.sh"
        unterminated.write_text(base.replace(sha_line, 'TERMUX_PKG_SHA256="unterminated\n' + sha_line), encoding="utf-8")
        run_recipe(unterminated)
        invalid_sha = work / "invalid-sha.sh"
        invalid_sha.write_text(base.replace(sha_line, "TERMUX_PKG_SHA256=\n"), encoding="utf-8")
        run_recipe(invalid_sha, expect_success=False)
        debian_path = work / "debian-path.sh"
        debian_path.write_text(base.replace("$TERMUX_PREFIX/bin/milena", "/usr/bin/milena"), encoding="utf-8")
        run_recipe(debian_path, expect_success=False)

        official = work / "official"
        official.mkdir()
        run_recipe(RECIPE, "--official-dir", str(official), expect_success=False)
        build_script = official / "build-package.sh"
        build_script.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        build_script.chmod(0o644)
        run_recipe(RECIPE, f"--official-dir={official}", expect_success=False)
        build_script.chmod(0o755)
        run_recipe(RECIPE, "--official-dir", str(official))

        if shutil.which("curl"):
            archive = work / "archive/refs/tags/v0.2.0.tar.gz"
            archive.parent.mkdir(parents=True)
            payload = b"local fixture for optional recipe SHA256 fetch\n"
            archive.write_bytes(payload)
            file_recipe = work / "file-fetch.sh"
            url = archive.as_uri().replace("v0.2.0.tar.gz", "v${TERMUX_PKG_VERSION}.tar.gz")
            fetch_source = base.replace(
                "TERMUX_PKG_SRCURL=https://github.com/46Neon/Milena/archive/refs/tags/v${TERMUX_PKG_VERSION}.tar.gz",
                f'TERMUX_PKG_SRCURL="{url}"',
            )
            digest = hashlib.sha256(payload).hexdigest()
            fetch_source = fetch_source.replace(sha_line, f'TERMUX_PKG_SHA256="{digest}"')
            file_recipe.write_text(fetch_source, encoding="utf-8")
            run_recipe(file_recipe, "--fetch")
            mismatch_recipe = work / "file-fetch-mismatch.sh"
            mismatch_recipe.write_text(fetch_source.replace(digest, "0" * 64), encoding="utf-8")
            run_recipe(mismatch_recipe, "--fetch", expect_success=False)
    with tempfile.TemporaryDirectory(prefix="milena-termux-boundary-") as raw:
        work = Path(raw)
        package = make_package(work)
        checksum = work / (package.name + ".sha256")
        checksum.write_text(f"{hashlib.sha256(package.read_bytes()).hexdigest()}  {package}\n", encoding="utf-8")
        run_validator(package, "--checksum", str(checksum))

        wrong_arch = make_package(work / "wrong-arch", architecture="arm64")
        run_validator(wrong_arch, expect_success=False)
        wrong_path = make_package(work / "wrong-path", debian_path=True)
        run_validator(wrong_path, expect_success=False)

        apt = work / "apt"
        pool = apt / "pool/main/m/milena"
        pool.mkdir(parents=True)
        copied = pool / package.name
        copied.write_bytes(package.read_bytes())
        index_dir = apt / "dists/stable/main/binary-aarch64"
        index_dir.mkdir(parents=True)
        digest = hashlib.sha256(copied.read_bytes()).hexdigest()
        size = copied.stat().st_size
        index = (
            "Package: milena\nVersion: 0.2.0\nArchitecture: aarch64\n"
            f"Filename: pool/main/m/milena/{package.name}\nSize: {size}\nSHA256: {digest}\n"
        )
        (index_dir / "Packages").write_text(index, encoding="utf-8")
        (index_dir / "Packages.gz").write_bytes(gzip.compress(index.encode(), mtime=0))
        packages_digest = hashlib.sha256((index_dir / "Packages.gz").read_bytes()).hexdigest()
        release = (
            "Origin: Milena\nSuite: stable\nComponents: main\n"
            "Architectures: aarch64\nSHA256:\n"
            f" {packages_digest} {(index_dir / 'Packages.gz').stat().st_size} main/binary-aarch64/Packages.gz\n"
        )
        (apt / "dists/stable").mkdir(exist_ok=True)
        (apt / "dists/stable/Release").write_text(release, encoding="utf-8")
        (apt / "dists/stable/InRelease").write_text("fixture signature placeholder\n", encoding="utf-8")
        (apt / "dists/stable/Release.gpg").write_bytes(b"fixture signature placeholder\n")
        (apt / "milena-archive-keyring.asc").write_text("fixture key placeholder\n", encoding="utf-8")
        run_validator(package, "--apt-root", str(apt))
    print("Termux packaging boundary tests: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
