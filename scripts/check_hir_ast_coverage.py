#!/usr/bin/env python3
"""Check that every current AST kind is classified at the HIR boundary."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AST_HEADER = (ROOT / "include/ast.h").read_text(encoding="utf-8")
HIR_SOURCE = (ROOT / "src/canonical_compiler.c").read_text(encoding="utf-8")
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

hir_entry = function_body(HIR_SOURCE, "MilenaStatus milena_canonical_hir_input(")
if "MILENA_ERR_UNSUPPORTED" not in hir_entry or "hir_first_unsupported_node" not in hir_entry:
    fail("the public HIR-only entry point must fail closed and locate an unsupported node")

print(
    "HIR AST coverage: "
    f"{len(represented)} represented kinds; {len(rejected)} explicitly rejected kinds; "
    "all ASTNodeType values classified."
)
