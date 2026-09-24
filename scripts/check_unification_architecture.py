#!/usr/bin/env python3
"""Enforce Milena's sole canonical language route and allowlisted adapters."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def fail(message):
    raise SystemExit(f"unification architecture check failed: {message}")


def read(path):
    file = ROOT / path
    if not file.is_file():
        fail(f"required route file is missing: {path}")
    return file.read_text(encoding="utf-8")


def need(ok, message):
    if not ok:
        fail(message)


def body(source, marker, label):
    start = source.find(marker)
    if start < 0:
        fail(f"missing {label} entrypoint: {marker!r}")
    opening = source.find("{", start)
    depth, state, i = 0, "code", opening
    while i < len(source):
        c, n = source[i], source[i + 1] if i + 1 < len(source) else ""
        if state == "line":
            if c == "\n": state = "code"
        elif state == "block":
            if c == "*" and n == "/": state, i = "code", i + 1
        elif state in ("string", "char"):
            if c == "\\": i += 1
            elif c == ('"' if state == "string" else "'"): state = "code"
        elif c == "/" and n == "/": state, i = "line", i + 1
        elif c == "/" and n == "*": state, i = "block", i + 1
        elif c == '"': state = "string"
        elif c == "'": state = "char"
        elif c == "{": depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0: return source[opening + 1:i]
        i += 1
    fail(f"unterminated {label} body")


def ordered(text, stages, label):
    at = -1
    for stage in stages:
        at = text.find(stage, at + 1)
        if at < 0: fail(f"{label} is missing or reorders stage {stage!r}")


make = read("Makefile")
match = re.search(r"(?ms)^SOURCES\s*=\s*(.*?)(?=^OBJECTS\s*=)", make)
need(match, "cannot read canonical product source list")
sources = re.findall(r"(?:src|third_party)/[\w./-]+\.c", match.group(1))
first_party = [p for p in sources if p.startswith("src/")]
entrypoints = [p for p in first_party if Path(p).stem == "main" or Path(p).stem.endswith("_main")]
parsers = [p for p in first_party if Path(p).name.endswith("parser.c")]
need(entrypoints == ["src/main.c"], f"parallel product entrypoint found: {entrypoints}")
need(parsers == ["src/parser.c"], f"parallel language parser found: {parsers}")
need(first_party.count("src/lexer.c") == 1, "product must compile exactly one canonical lexer")
for p in ("src/main.c", "src/entrypoints.c", "src/script.c", "src/lexer.c", "src/parser.c",
          "src/ast.c", "src/language_semantic.c", "src/language_runtime.c",
          "src/canonical_compiler.c", "src/query_plan.c"):
    need(sources.count(p) == 1, f"canonical pipeline source must occur once: {p}")
legacy = re.search(r"(?m)^FUNCTION_OBJECTS\s*=\s*(.*)$", make)
need(legacy, "legacy function-parser boundary disappeared")
objects = re.findall(r"src/[\w./-]+\.o", legacy.group(1))
need(objects == ["src/function_parser.o", "src/user_functions.o"], f"unexpected compatibility objects: {objects}")

main = read("src/main.c")
for token in ("lexer_init(", "parser_init(", "parser_parse(", "dataset_load_csv(", "milena_run_dataset_program("):
    need(token not in main, f"main.c contains a parallel frontend/backend: {token}")
for command in ("analyze", "profile", "inspect", "run_script"):
    need(f"milena_cli_{command}" in main, f"CLI command bypasses its entrypoint adapter: {command}")
entry = read("src/entrypoints.c")
ordered(body(entry, "cli_frontend(", "CLI frontend"),
        ("lexer_init(", "parser_init(", "parser_parse(", "milena_validate_ast("), "CLI frontend")
ordered(body(entry, "cli_load_dataset(", "CLI dataset adapter"),
        ("cli_frontend(", "dataset_load_csv("), "CLI dataset adapter")

script = read("src/script.c")
for token in ("script_pipeline_from_ast", "script_pipeline_for_source", "SCRIPT_PIPELINE_PARSE_ERROR",
              "script_has_canonical_marker", "milena_run_array_program", "milena_run_dataset_program",
              "run_canonical_functions", "run_legacy_numeric_functions",
              "Desde aquí comienza únicamente la ruta histórica de compatibilidad"):
    need(token in script, f"script router lost explicit boundary: {token}")
router = body(script, "MilenaStatus milena_run_script(", "script runtime")
ordered(router, ("script_pipeline_for_source(", "SCRIPT_PIPELINE_PARSE_ERROR",
                 "SCRIPT_PIPELINE_CANONICAL_ARRAY", "SCRIPT_PIPELINE_CANONICAL_DATASET",
                 "Desde aquí comienza únicamente la ruta histórica de compatibilidad", "parse_schema("),
        "script router")
legacy_body = body(script, "static MilenaStatus run_legacy_numeric_functions(", "legacy function adapter")
need("milena_parse_numeric_functions(" in legacy_body, "legacy parser escaped its compatibility adapter")

runtime = read("src/language_runtime.c")
data = body(runtime, "MilenaStatus milena_run_dataset_program(", "canonical dataset runtime")
ordered(data, ("lexer_init(", "parser_init(", "parser_parse(", "milena_validate_ast("), "dataset frontend")
for token in ("milena_stream_execution_plan_build(", "milena_arrow_ipc_execution_plan_build(",
              "milena_sql_execution_plan_build(", "milena_canonical_program_parse(",
              "milena_canonical_program_bind_table(", "milena_canonical_compiler_input(",
              "milena_canonical_program_execute_data(", "milena_sql_run_plan(",
              "milena_arrow_ipc_stream_transform(", "run_stream_dataset_with_options("):
    need(token in data, f"canonical runtime bypasses a typed planner/runtime stage: {token}")
ordered(data, ("milena_validate_ast(", "milena_canonical_program_parse(",
               "milena_canonical_program_bind_table(", "milena_canonical_compiler_input(",
               "milena_canonical_program_execute_data("), "data-HIR runtime")
plans = read("src/query_plan.c")
for token in ("milena_stream_execution_plan_validate(plan, error)",
              "milena_arrow_ipc_execution_plan_validate(plan, error)",
              "milena_sql_execution_plan_validate(plan, error)", "MILENA_LOGICAL_CSV_SCAN",
              "MILENA_LOGICAL_JSON_REPORT"):
    need(token in plans, f"typed planner lacks validation/operator: {token}")
need("main(" not in plans, "query planner must not define another executable")
hir = read("src/canonical_compiler.c")
for token in ("milena_canonical_program_parse(", "data_hir_build(",
              "milena_canonical_program_execute_data(", "MILENA_ERR_UNSUPPORTED"):
    need(token in hir, f"canonical HIR boundary is incomplete: {token}")

need(re.search(r"(?m)^check-unification-architecture:\s*check-hir-ast-coverage\s*$", make),
     "architecture check must depend on closed-HIR coverage")
need(re.search(r"(?m)^test:.*check-unification-architecture", make),
     "make test must execute the architecture check")
contract = read("docs/CANONICAL_PIPELINE_CONTRACT.md")
for token in ("## Ruta canónica", "## Ramas tipadas actuales", "## Excepciones de compatibilidad",
              "## Límites que siguen abiertos", "## Tres reglas no negociables",
              "lexer → parser → AST tipado → semántica → HIR → plan físico → runtime/backend",
              "function_parser.c", "script.c", "entrypoints.c", "rehace el parseo", "HIR universal"):
    need(token in contract, f"pipeline contract omits boundary: {token}")

print("OK: una entrada, un lexer/parser, HIR cerrado, planificadores tipados y compatibilidad explícita")
