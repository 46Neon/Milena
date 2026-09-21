#!/usr/bin/env python3
"""Fail-closed ELF boundary check for a native Termux/aarch64 binary."""
from __future__ import annotations
import subprocess
import sys
from pathlib import Path

FORBIDDEN = ("libc.so.6", "ld-linux", "libpthread.so.0", "libstdc++.so.6", "GLIBC_")

def output(*args: str) -> str:
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)

def main() -> int:
    if len(sys.argv) != 2:
        print("usage: validate_termux_elf.py BINARY", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1])
    if not binary.is_file():
        print(f"ERROR: missing binary: {binary}", file=sys.stderr)
        return 1
    try:
        header = output("readelf", "-h", str(binary))
        program = output("readelf", "-l", str(binary))
        dynamic = output("readelf", "-d", str(binary))
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: readelf failed: {exc}", file=sys.stderr)
        return 1
    required = (("Class:", "ELF64"), ("Data:", "little endian"), ("Machine:", "AArch64"))
    for label, value in required:
        if value.lower() not in header.lower():
            print(f"ERROR: ELF is not native aarch64 ({label} {value})", file=sys.stderr)
            return 1
    if "/system/bin/linker64" not in program:
        print("ERROR: ELF does not use the bionic aarch64 interpreter /system/bin/linker64", file=sys.stderr)
        return 1
    if "libc.so" not in dynamic:
        print("ERROR: ELF is not dynamically linked against bionic libc.so", file=sys.stderr)
        return 1
    for forbidden in FORBIDDEN:
        if forbidden in dynamic or forbidden in header or forbidden in program:
            print(f"ERROR: glibc/host dependency detected: {forbidden}", file=sys.stderr)
            return 1
    print(f"Termux ELF boundary: OK ({binary})")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
