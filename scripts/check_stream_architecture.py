#!/usr/bin/env python3
"""Proof that streaming remains a language/runtime backend, not a parallel tool."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ast = (ROOT / "include/ast.h").read_text()
parser = (ROOT / "src/parser.c").read_text()
runtime = (ROOT / "src/language_runtime.c").read_text()
make = (ROOT / "Makefile").read_text()
manifest = (ROOT / "scripts/check_source_manifest.py").read_text()

required = [
    "ASTStreamOperation", "stream_chunk_rows", "stream_record_limit",
    "stream_column_limit", "stream_operation_from_token",
]
for marker in required:
    if marker not in ast + parser:
        raise SystemExit(f"missing typed AST contract: {marker}")
if "milena_validate_ast(program, error)" not in runtime:
    raise SystemExit("runtime bypasses semantic validation")
if "run_stream_dataset_with_options" not in runtime or "milena_stream_csv_summary_with_options" not in runtime:
    raise SystemExit("stream backend is not invoked by the canonical runtime")
# The natural syntax branch must consume typed AST operations, not scan text.
start = runtime.index("static MilenaStatus run_stream_dataset_with_options")
end = runtime.index("MilenaStatus milena_run_dataset_program", start)
stream_runtime = runtime[start:end]
if "summary->stream_operation" not in stream_runtime:
    raise SystemExit("natural stream metrics do not use typed AST operations")
if "sscanf(summary->value" in stream_runtime:
    raise SystemExit("natural stream metrics still use textual scanning")
if "src/stream.c" not in make or '"stream.c"' not in manifest:
    raise SystemExit("stream.c is not classified as official product")
if re.search(r"stream[^\n]*main\s*\(", (ROOT / "src/stream.c").read_text()):
    raise SystemExit("stream.c contains a standalone entry point")
print("OK: streaming architecture is typed, canonical, and product-classified")
