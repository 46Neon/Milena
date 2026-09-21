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
    "stream_column_limit", "stream_operation_from_token", "AST_BLOQUE_AGRUPAR", "AST_AGRUPACION_POR",
]
for marker in required:
    if marker not in ast + parser:
        raise SystemExit(f"missing typed AST contract: {marker}")
if "milena_validate_ast(program, error)" not in runtime:
    raise SystemExit("runtime bypasses semantic validation")
if "run_stream_dataset_with_options" not in runtime or "milena_stream_csv_summary_with_options" not in runtime or "milena_stream_csv_grouped_with_options" not in runtime:
    raise SystemExit("grouped stream backend is not invoked by the canonical runtime")
    raise SystemExit("stream backend is not invoked by the canonical runtime")
# The natural syntax branch must consume typed AST operations, not scan text.
start = runtime.index("static MilenaStatus run_stream_dataset_with_options")
end = runtime.index("MilenaStatus milena_run_dataset_program", start)
stream_runtime = runtime[start:end]
if "summary->stream_operation" not in stream_runtime:
    raise SystemExit("natural stream metrics do not use typed AST operations")
if "sscanf(summary->value" in stream_runtime:
    raise SystemExit("natural stream metrics still use textual scanning")
if "max_groups" not in runtime or "STREAM_HARD_MAX_GROUPS" not in (ROOT / "src/stream.c").read_text():
    raise SystemExit("grouped stream cap is not enforced")
if "src/stream.c" not in make or '"stream.c"' not in manifest:
    raise SystemExit("stream.c is not classified as official product")
# Related analysis, SST, and finance remain language-runtime capabilities.
for marker in ("AST_COMANDO_SST", "runtime_write_sst", "milena_simple_interest", "src/finance.c"):
    if marker not in runtime + make:
        raise SystemExit(f"canonical analysis capability missing: {marker}")
product_sources = make.split("SOURCES =", 1)[1].split("OBJECTS", 1)[0]
for experimental in ("compiler.c", "ir.c", "vm.c", "gc.c", "arena.c"):
    # Match complete source tokens: canonical_compiler.c is an intentional
    # adapter and must not be mistaken for the experimental compiler.c.
    if re.search(rf"(?<![A-Za-z0-9_]){re.escape(experimental)}(?![A-Za-z0-9_])", product_sources):
        raise SystemExit(f"experimental module leaked into product build: {experimental}")
if re.search(r"stream[^\n]*main\s*\(", (ROOT / "src/stream.c").read_text()):
    raise SystemExit("stream.c contains a standalone entry point")
print("OK: streaming architecture is typed, canonical, and product-classified")
