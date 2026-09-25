#!/usr/bin/env python3
"""Check that every current AST kind is classified at the HIR boundary."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AST_HEADER = (ROOT / "include/ast.h").read_text(encoding="utf-8")
HIR_SOURCE = (ROOT / "src/canonical_compiler.c").read_text(encoding="utf-8")
QUERY_PLAN_SOURCE = (ROOT / "src/query_plan.c").read_text(encoding="utf-8")
HIR_DOC = (ROOT / "docs/COMPILADOR_IR_PLAN.md").read_text(encoding="utf-8")


def fail(message: str) -> None:
    raise SystemExit(f"HIR AST coverage check failed: {message}")


def function_body(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        fail(f"missing implementation marker {marker!r}")
    opening = source.find("{", start)
    if opening < 0:
        fail(f"missing function body for {marker!r}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    fail(f"unterminated function body for {marker!r}")
    return ""


enum_match = re.search(
    r"typedef\s+enum\s*\{(?P<body>.*?)\}\s*ASTNodeType\s*;",
    AST_HEADER,
    re.DOTALL,
)
if not enum_match:
    fail("could not locate ASTNodeType enum")
ast_nodes = set(re.findall(r"\bAST_[A-Z0-9_]+\b", enum_match.group("body")))
ast_nodes.discard("AST_NODE_TYPE_COUNT")

represented_start = HIR_DOC.find("El subconjunto representado por el HIR escalar es:")
rejected_start = HIR_DOC.find("Todos los demás tipos declarados en `ASTNodeType`", represented_start)
rejected_end = HIR_DOC.find("Esto cubre las familias", rejected_start)
if min(represented_start, rejected_start, rejected_end) < 0:
    fail("documentation must keep explicit represented/rejected AST inventories")
represented = set(re.findall(
    r"\bAST_[A-Z0-9_]+\b", HIR_DOC[represented_start:rejected_start]
))
rejected = set(re.findall(
    r"\bAST_[A-Z0-9_]+\b", HIR_DOC[rejected_start:rejected_end]
))
if represented & rejected:
    fail(f"AST nodes classified both represented and rejected: {sorted(represented & rejected)}")
if represented | rejected != ast_nodes:
    fail(
        "documentation does not classify the exact AST enum; "
        f"unclassified={sorted(ast_nodes - represented - rejected)}, "
        f"unknown={sorted((represented | rejected) - ast_nodes)}"
    )

eligibility = function_body(HIR_SOURCE, "static bool hir_supports_ast_node(")
eligibility_nodes = set(re.findall(r"\bcase\s+(AST_[A-Z0-9_]+)\s*:", eligibility))
if eligibility_nodes != represented:
    fail(
        "documented HIR subset differs from the explicit eligibility allowlist; "
        f"missing={sorted(represented - eligibility_nodes)}, "
        f"extra={sorted(eligibility_nodes - represented)}"
    )
if "default:" not in eligibility or "return false;" not in eligibility:
    fail("unknown AST kinds must be rejected by the eligibility default")

# The data HIR is a separate closed subset. Keep its builder, strict-entry
# allowlist, and documented inventory in lockstep as operations are added.
data_marker = "La matriz de nodos realmente recorridos por el builder de datos es:"
data_start = HIR_DOC.find(data_marker)
data_end = HIR_DOC.find("Todo AST restante está fuera de la HIR de datos", data_start)
if min(data_start, data_end) < 0:
    fail("documentation must list the exact data-HIR builder inventory")
data_represented = set(re.findall(
    r"\bAST_[A-Z0-9_]+\b", HIR_DOC[data_start:data_end]
))
if not data_represented <= ast_nodes:
    fail(f"data-HIR inventory contains unknown AST nodes: {sorted(data_represented - ast_nodes)}")
data_eligibility = function_body(
    HIR_SOURCE, "static bool hir_supports_data_ast_node("
)
data_eligibility_nodes = set(re.findall(
    r"\bcase\s+(AST_[A-Z0-9_]+)\s*:", data_eligibility
))
if data_eligibility_nodes != data_represented:
    fail(
        "documented data-HIR subset differs from its strict-entry allowlist; "
        f"missing={sorted(data_represented - data_eligibility_nodes)}, "
        f"extra={sorted(data_eligibility_nodes - data_represented)}"
    )
if "default:" not in data_eligibility or "return false;" not in data_eligibility:
    fail("unknown data AST kinds must be rejected by the eligibility default")
data_builder = function_body(
    HIR_SOURCE,
    "static HIRBuildResult data_hir_build(const ASTNode *ast, MilenaDataHIR **output) {",
)
data_builder_nodes = set(re.findall(r"\bAST_[A-Z0-9_]+\b", data_builder))
if data_builder_nodes != data_represented:
    fail(
        "documented data-HIR subset differs from AST nodes handled by the builder; "
        f"missing={sorted(data_represented - data_builder_nodes)}, "
        f"extra={sorted(data_builder_nodes - data_represented)}"
    )

expression_builder = function_body(
    HIR_SOURCE, "static MilenaHIRExpression *hir_build_expression(const ASTNode *node,"
)
statement_builder = function_body(
    HIR_SOURCE,
    "static MilenaHIRStatement *hir_build_statement(const ASTNode *node,\n"
    "                                               HIRBuildResult *result) {",
)
expression_nodes = set(re.findall(r"\bcase\s+(AST_[A-Z0-9_]+)\s*:", expression_builder))
statement_nodes = set(re.findall(r"\bcase\s+(AST_[A-Z0-9_]+)\s*:", statement_builder))
if not expression_nodes | statement_nodes <= represented:
    fail("an expression/statement builder case is absent from the represented inventory")
if "node->type != AST_DECLARACION_FUNCION" not in HIR_SOURCE:
    fail("function declarations must remain handled by the scalar HIR builder")

arrow_builder = function_body(
    QUERY_PLAN_SOURCE,
    "MilenaStatus milena_arrow_ipc_execution_plan_build("
)
arrow_cases = set(re.findall(r"\bcase\s+(AST_[A-Z0-9_]+)\s*:", arrow_builder))
expected_arrow_cases = {
    "AST_LLAMADA_CARGAR", "AST_COLUMNAR_PROJECT", "AST_STREAM_FILTER",
    "AST_BLOQUE_EXPORTAR", "AST_DECLARACION_VARIABLE",
}
if arrow_cases != expected_arrow_cases:
    fail(
        "Arrow IPC plan cases differ from the audited contract; "
        f"missing={sorted(expected_arrow_cases - arrow_cases)}, "
        f"extra={sorted(arrow_cases - expected_arrow_cases)}"
    )
if "AST_COLUMNAR_FIELD" not in arrow_builder or \
   "MILENA_ERR_UNSUPPORTED" not in arrow_builder or "default:" not in arrow_builder:
    fail("Arrow IPC plan must validate projected fields and reject unknown AST nodes")
canonical_parse = function_body(
    HIR_SOURCE, "MilenaStatus milena_canonical_program_parse("
)
if "milena_arrow_ipc_execution_plan_build" not in canonical_parse or \
   "program->arrow_plan = arrow_plan" not in canonical_parse:
    fail("the canonical parser must own the validated Arrow IPC typed plan")
compatibility_entry = function_body(
    HIR_SOURCE, "MilenaStatus milena_canonical_compatibility_input("
)
if "input->arrow_plan = program->arrow_plan" not in compatibility_entry:
    fail("the compiler input must expose the Arrow plan as a borrowed view")
hir_entry = function_body(HIR_SOURCE, "MilenaStatus milena_canonical_hir_input(")
if "MILENA_ERR_UNSUPPORTED" not in hir_entry or "hir_first_unsupported_node" not in hir_entry:
    fail("the public HIR-only entry point must fail closed and locate an unsupported node")
if "if (!program->hir && !program->data_hir)" not in hir_entry:
    fail("an Arrow plan must not bypass the strict HIR lowering requirement")
if "milena_canonical_compatibility_input(program, input, error)" not in hir_entry:
    fail("the strict HIR entry must delegate only after a real HIR exists")

print(
    "HIR AST coverage: "
    f"scalar={len(represented)} represented/{len(rejected)} outside scalar HIR; "
    f"data={len(data_represented)} represented/{len(ast_nodes - data_represented)} outside data HIR; "
    f"Arrow plan={len(expected_arrow_cases) + 1} typed node kinds; "
    "all ASTNodeType values classified in the closed subsets."
)
