#!/usr/bin/env python3
"""Proof that streaming remains a language/runtime backend, not a parallel tool."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ast = (ROOT / "include/ast.h").read_text()
parser = (ROOT / "src/parser.c").read_text()
runtime = (ROOT / "src/language_runtime.c").read_text()
planner = (ROOT / "src/query_plan.c").read_text()
make = (ROOT / "Makefile").read_text()
manifest = (ROOT / "scripts/check_source_manifest.py").read_text()
stream = (ROOT / "src/stream.c").read_text()
stream_header = (ROOT / "include/stream.h").read_text()
streaming_docs = (ROOT / "docs/STREAMING_EXECUTION.md").read_text()
spill_contract = (ROOT / "docs/GROUPED_SPILL_CONTRACT.md").read_text()
pr25_docs = (ROOT / "docs/PR25_BIG_DATA_FOUNDATION.md").read_text()

required = [
    "ASTStreamOperation", "stream_chunk_rows", "stream_record_limit",
    "stream_column_limit", "stream_group_limit", "stream_operation_from_token",
    "parse_stream_group", "AST_BLOQUE_AGRUPAR",
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
if "milena_stream_csv_grouped_with_options" not in stream_runtime:
    raise SystemExit("grouped streaming is not invoked by the canonical runtime")
if ("AST_AGRUPACION_POR" not in planner or "plan->group_key" not in stream_runtime or
        "group_key->value" not in stream_runtime or
        "MILENA_PHYSICAL_CSV_STREAM_GROUPED" not in stream_runtime):
    raise SystemExit("grouping key bypasses the typed logical/physical plan")
if "milena_stream_execution_plan_build" not in runtime:
    raise SystemExit("canonical runtime bypasses the typed streaming plan")
if "milena_stream_csv_grouped_with_options" not in stream_header:
    raise SystemExit("grouped streaming API is not declared in the canonical contract")
# Do not let docs imply a spill implementation before a typed backend, source
# manifest entry, runtime path, and end-to-end evidence actually exist.
if "src/spill.c" in make or "src/spill_store.c" in make:
    raise SystemExit("planned spill module must be implemented and integrated before entering SOURCES")
if "no hay spill-to-disk" not in streaming_docs.lower():
    raise SystemExit("streaming docs must state that grouped spill is not implemented")
if "no implementa spill-to-disk" not in pr25_docs.lower():
    raise SystemExit("PR25 status must state that grouped spill is not implemented")
for marker in ("contrato", "checksum", "límites", "limpieza", "e2e"):
    if marker not in spill_contract.lower():
        raise SystemExit(f"grouped spill design contract is incomplete: {marker}")
for marker in ("STREAM_HARD_MAX_GROUPS", "STREAM_GROUP_STATE_BUDGET", "qsort(groups"):
    if marker not in stream:
        raise SystemExit(f"bounded/deterministic grouping guard missing: {marker}")
if "sscanf(summary->value" in stream_runtime:
    raise SystemExit("natural stream metrics still use textual scanning")
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
print("OK: global and grouped streaming use typed AST, canonical runtime, bounded state, and product sources")
