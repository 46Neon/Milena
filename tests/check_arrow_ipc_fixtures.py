#!/usr/bin/env python3
"""Verify checked-in Arrow IPC fixture provenance fingerprints without PyArrow."""
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "fixtures" / "arrow_ipc"
FIXTURES = {
    "generated_primitive_cpp21.stream": (
        7152,
        "124a70f6607c24dbdb50080ba953bb0f3156e3f8f4d0ea48f58b36f86c6b6e5d",
    ),
    "generated_binary_cpp21.stream": (
        13392,
        "d284f820575fdc28adaba6808a2499446ed50672122f6a47742065b0541f24d4",
    ),
    "int64_above_2_53_pyarrow23.stream": (
        480,
        "595811b8fc65f05b2972befa9a9dbde7a96ce08102b994993e80d469d50115f5",
    ),
}


def main() -> None:
    for name, (expected_size, expected_hash) in FIXTURES.items():
        data = (ROOT / name).read_bytes()
        actual_hash = sha256(data).hexdigest()
        if len(data) != expected_size or actual_hash != expected_hash:
            raise SystemExit(
                f"Fixture mismatch: {name}: {len(data)} bytes, {actual_hash}"
            )
    print(f"OK: {len(FIXTURES)} Arrow IPC fixture hashes")


if __name__ == "__main__":
    main()
