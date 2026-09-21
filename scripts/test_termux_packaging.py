#!/usr/bin/env python3
"""Reproducible boundary tests for the Termux/APT validator.

The fixture is deliberately a metadata-only package. It is not a Termux build
and this test must never be described as an install or execution test on
Android.
"""
from __future__ import annotations

import gzip
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "scripts/validate_termux_artifact.py"
RECIPE_VALIDATOR = ROOT / "scripts/validate_termux_recipe.py"
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
        "Package: milena\nVersion: 0.1.1\n"
        f"Architecture: {architecture}\nMaintainer: Milena Fixture <fixture@example.invalid>\nDescription: fixture\n",
        encoding="utf-8",
    )
    package = directory / "milena_0.1.1_aarch64.deb"
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


def run_recipe(path: Path, expect_success: bool = True) -> None:
    result = subprocess.run(
        [sys.executable, str(RECIPE_VALIDATOR), str(path)],
        cwd=ROOT, text=True, capture_output=True,
    )
    if (result.returncode == 0) != expect_success:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise AssertionError(f"recipe validator success={result.returncode == 0}, expected {expect_success}")


def main() -> int:
    run_recipe(RECIPE)
    with tempfile.TemporaryDirectory(prefix="milena-termux-recipe-") as raw_recipe:
        base = RECIPE.read_text(encoding="utf-8")
        missing_sha = Path(raw_recipe) / "missing-sha.sh"
        missing_sha.write_text(base.replace(
            "TERMUX_PKG_SHA256=21eb3cba83916e24198a68ed8f783442efbe4d01ca2236e36662d958b10d60de",
            "TERMUX_PKG_SHA256=",
        ), encoding="utf-8")
        run_recipe(missing_sha, expect_success=False)
        debian_path = Path(raw_recipe) / "debian-path.sh"
        debian_path.write_text(base.replace("$TERMUX_PREFIX/bin/milena", "/usr/bin/milena"), encoding="utf-8")
        run_recipe(debian_path, expect_success=False)
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
            "Package: milena\nVersion: 0.1.1\nArchitecture: aarch64\n"
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
