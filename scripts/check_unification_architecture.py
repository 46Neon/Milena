#!/usr/bin/env python3
"""Structural guardrails for the PR24 unification boundary."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
main = (ROOT / "src/main.c").read_text(encoding="utf-8")
entry = (ROOT / "src/entrypoints.c").read_text(encoding="utf-8")

# The CLI must not carry a second Dataset/analysis implementation.
for forbidden in ("dataset_load_csv", "dataset_print", "analysis_sales", "analysis_dataset_report"):
    assert forbidden not in main, f"main.c contiene lógica directa: {forbidden}"
for required in ("milena_cli_analyze", "milena_cli_profile", "milena_cli_inspect"):
    assert required in main, f"falta adaptador canónico: {required}"

# Every compatibility entrypoint must first go through the canonical frontend.
for required in ("lexer_init", "parser_init", "parser_parse", "milena_validate_ast"):
    assert required in entry, f"entrypoints.c no valida por el pipeline: {required}"

# PR24 has one streaming backend and no executable/parser fork.
stream = (ROOT / "src/stream.c").read_text(encoding="utf-8")
assert "milena_stream_csv_summary_with_options" in stream
assert not (ROOT / "src/stream_main.c").exists()

print("OK: CLI y flujo respetan la frontera lexer-parser-AST-semántica-runtime-backend")
