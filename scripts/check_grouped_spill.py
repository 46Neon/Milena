#!/usr/bin/env python3
"""Guard canonical typed grouped spill wiring and bounded cleanup."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
header = (ROOT / "include/stream.h").read_text()
ast = (ROOT / "include/ast.h").read_text()
parser = (ROOT / "src/parser.c").read_text()
semantic = (ROOT / "src/language_semantic.c").read_text()
runtime = (ROOT / "src/language_runtime.c").read_text()
stream = (ROOT / "src/stream.c").read_text()
backend_test = (ROOT / "tests/test_stream.c").read_text()
format_test = (ROOT / "tests/test_stream_format.c").read_text()
e2e_test = (ROOT / "tests/test_language_runtime.c").read_text()

for marker in ("spill_directory", "max_spill_bytes", "max_resident_groups"):
    if marker not in header:
        raise SystemExit(f"missing bounded backend option: {marker}")
for marker in ("stream_spill_directory", "stream_spill_disk_limit", "stream_resident_group_limit"):
    if marker not in ast:
        raise SystemExit(f"missing typed AST spill field: {marker}")
for marker in ("temporales", "disco", "residentes", "stream_spill_disk_limit"):
    if marker not in parser:
        raise SystemExit(f"missing language-level spill syntax: {marker}")
for marker in ("stream_spill_directory", "stream_spill_disk_limit", "has_grouped_stream"):
    if marker not in semantic:
        raise SystemExit(f"missing spill semantic validation: {marker}")
for marker in ("options.spill_directory", "options.max_spill_bytes", "max_resident_groups"):
    if marker not in runtime:
        raise SystemExit(f"runtime does not plumb typed spill policies: {marker}")
for marker in ("mkstemp", "stream_spill_directory_private", "spill_hash64",
               "spill_run_header_write", "spill_cursor_next", "stream_spill_merge_next",
               "rename(output_temp_path, output_path)", "unlink(spill_runs[r].path)"):
    if marker not in stream:
        raise SystemExit(f"missing bounded spill/cleanup implementation: {marker}")
if "tmpfile()" in stream:
    raise SystemExit("spill runs must be named private files in the explicit directory")
for marker in ("spill_runs", "max_spill_bytes = 16", "rmdir(spill_directory)", "max_resident_groups"):
    if marker not in backend_test:
        raise SystemExit(f"missing backend spill budget/cleanup/repeated-run test: {marker}")
for marker in ("stream_format.run", "header checksum", "spill_results", "grupos residentes"):
    if marker not in format_test + e2e_test:
        raise SystemExit(f"missing format or canonical language E2E evidence: {marker}")
print("OK: grouped spill is typed through lexer/parser/AST/semantic/runtime and has bounded backend tests")
