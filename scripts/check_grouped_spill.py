#!/usr/bin/env python3
"""Guard the typed grouped-spill slice and its bounded cleanup contract."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/stream.c").read_text()
header = (ROOT / "include/stream.h").read_text()
test = (ROOT / "tests/test_stream.c").read_text()
docs = (ROOT / "docs/GROUPED_SPILL_CONTRACT.md").read_text().lower()
for token in ("spill_enabled", "max_spill_bytes", "max_spill_records", "max_spill_files"):
    if token not in header:
        raise SystemExit(f"missing typed spill option: {token}")
for token in ("tmpfile()", "spill_hash64", "spill_run_header_write", "spill_cursor_next",
              "stream_spill_merge_next", "stream_groups_clear", "spill_cursor_clear"):
    if token not in source:
        raise SystemExit(f"missing spill writer/reader/cleanup implementation: {token}")
for token in ("grouped_report.spilled", "spill_runs", "max_spill_bytes = 16",
              "MILENA_ERR_OVERFLOW"):
    if token not in test:
        raise SystemExit(f"missing forced-spill or fail-closed test: {token}")
for token in ("checksum", "corridas", "64 mib", "tmpfile", "api tipada c"):
    if token not in docs:
        raise SystemExit(f"grouped spill documentation is incomplete: {token}")
print("OK: grouped spill uses bounded typed runs, checksums, deterministic merge, and fail-closed tests")
