#!/usr/bin/env python3
"""Validate the AST-to-typed-IR inventory ledger against include/ast.h."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

EXPECTED_VARIANTS = 72
EXPECTED_CONTRACT_COUNTS = {
    "documented_syntax_contract": 47,
    "unresolved_public_contract": 21,
    "unresolved_enum_only_legacy_status": 4,
}
EXPECTED_PARTIAL = {
    "AST_DECLARACION_VARIABLE",
    "AST_ASIGNACION_VARIABLE",
    "AST_COMANDO_RETORNAR",
    "AST_CONDICION_SI",
    "AST_EXPRESION_LITERAL",
    "AST_EXPRESION_IDENTIFICADOR",
    "AST_EXPRESION_OPERACION",
}
SENTINEL = "AST_NODE_TYPE_COUNT"
LOWERING_STATUSES = {"full", "partial", "not_lowered"}
LANGUAGE_CONTRACT_STATUSES = {
    "documented_syntax_contract",
    "unresolved_public_contract",
    "unresolved_enum_only_legacy_status",
}
PARSER_STATUSES = {
    "parser_reachable_documented_contract",
    "parser_reachable_contract_unresolved",
    "no_current_parser_constructor_found_enum_only_legacy_unconfirmed",
}
PER_VARIANT_EVIDENCE_STATUSES = {
    "unresolved_per_variant",
    "partially_verified",
    "verified",
}
FULL_EVIDENCE_STAGES = {
    "language_contract",
    "parser",
    "semantic_analysis",
    "typed_ir_lowering",
    "portable_bytecode",
    "reference_execution",
    "differential_tests",
    "errors_spans_ownership_resources",
    "exact_sha_ci_platform_gates",
}


class LedgerError(ValueError):
    """An invalid header or coverage ledger."""


def reject_duplicate_json_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise LedgerError(f"duplicate JSON object key: {key}")
        result[key] = value
    return result


def ast_enum_variants(header_path: Path) -> list[str]:
    source = header_path.read_text(encoding="utf-8")
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    match = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*ASTNodeType\s*;", source, re.S)
    if not match:
        raise LedgerError(f"could not locate typedef enum ASTNodeType in {header_path}")

    names: list[str] = []
    for item in match.group(1).split(","):
        item = item.strip()
        if not item:
            continue
        enumerator = re.fullmatch(r"(AST_[A-Z0-9_]+)(?:\s*=\s*[^,]+)?", item, re.S)
        if not enumerator:
            raise LedgerError(f"unrecognized ASTNodeType enumerator: {item!r}")
        names.append(enumerator.group(1))

    duplicates = sorted(name for name in set(names) if names.count(name) > 1)
    if duplicates:
        raise LedgerError(f"duplicate ASTNodeType enumerator(s): {', '.join(duplicates)}")
    if names.count(SENTINEL) != 1:
        raise LedgerError(f"ASTNodeType must contain exactly one {SENTINEL}")
    if not names or names[-1] != SENTINEL:
        raise LedgerError(f"{SENTINEL} must be the final ASTNodeType enumerator")
    variants = names[:-1]
    if len(variants) != EXPECTED_VARIANTS:
        raise LedgerError(
            f"expected {EXPECTED_VARIANTS} real AST variants before {SENTINEL}; found {len(variants)}"
        )
    return variants


def require(condition: bool, message: str) -> None:
    if not condition:
        raise LedgerError(message)


def check_ledger(header_path: Path, ledger_path: Path) -> tuple[int, int, int, int]:
    header_variants = ast_enum_variants(header_path)
    try:
        ledger = json.loads(
            ledger_path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_json_keys,
        )
    except (json.JSONDecodeError, OSError) as exc:
        raise LedgerError(f"cannot read valid JSON ledger {ledger_path}: {exc}") from exc

    require(isinstance(ledger, dict), "ledger root must be a JSON object")
    require(ledger.get("schema_version") == 1, "ledger schema_version must be 1")
    require(ledger.get("sentinel") == SENTINEL, f"ledger sentinel must be {SENTINEL}")
    require(
        ledger.get("expected_real_variant_count") == EXPECTED_VARIANTS,
        f"ledger expected_real_variant_count must be {EXPECTED_VARIANTS}",
    )
    records = ledger.get("variants")
    require(isinstance(records, list), "ledger variants must be a JSON array")

    names: list[str] = []
    counts = {status: 0 for status in LOWERING_STATUSES}
    contract_counts = {status: 0 for status in LANGUAGE_CONTRACT_STATUSES}
    for index, record in enumerate(records):
        require(isinstance(record, dict), f"variants[{index}] must be an object")
        name = record.get("ast_variant")
        require(isinstance(name, str) and name, f"variants[{index}].ast_variant must be a name")
        names.append(name)
        require(name != SENTINEL, f"sentinel {SENTINEL} must not appear in ledger variants")

        lowering = record.get("lowering_status")
        require(
            isinstance(lowering, str) and lowering in LOWERING_STATUSES,
            f"{name}: invalid lowering_status {lowering!r}",
        )
        counts[lowering] += 1
        language = record.get("language_contract_status")
        require(
            isinstance(language, str) and language in LANGUAGE_CONTRACT_STATUSES,
            f"{name}: invalid language_contract_status {language!r}",
        )
        contract_counts[language] += 1
        parser = record.get("parser_status")
        require(
            isinstance(parser, str) and parser in PARSER_STATUSES,
            f"{name}: invalid parser_status {parser!r}",
        )
        expected_parser = {
            "documented_syntax_contract": "parser_reachable_documented_contract",
            "unresolved_public_contract": "parser_reachable_contract_unresolved",
            "unresolved_enum_only_legacy_status": (
                "no_current_parser_constructor_found_enum_only_legacy_unconfirmed"
            ),
        }[language]
        require(parser == expected_parser, f"{name}: parser status conflicts with contract classification")
        semantic_status = record.get("semantic_status")
        testing_status = record.get("testing_status")
        require(
            isinstance(semantic_status, str) and semantic_status in PER_VARIANT_EVIDENCE_STATUSES,
            f"{name}: invalid semantic_status {semantic_status!r}",
        )
        require(
            isinstance(testing_status, str) and testing_status in PER_VARIANT_EVIDENCE_STATUSES,
            f"{name}: invalid testing_status {testing_status!r}",
        )
        if lowering == "full":
            require(
                language == "documented_syntax_contract"
                and parser == "parser_reachable_documented_contract",
                f"{name}: full coverage requires a resolved language and parser contract",
            )
            require(
                semantic_status == "verified" and testing_status == "verified",
                f"{name}: full coverage requires verified semantic and testing status",
            )
        for field in ("inventory_evidence", "lowering_evidence"):
            require(
                isinstance(record.get(field), str) and record[field].strip(),
                f"{name}: {field} must cite non-empty evidence",
            )

        evidence = record.get("completion_evidence")
        if lowering == "full":
            require(isinstance(evidence, dict), f"{name}: full lowering requires completion_evidence")
            require(
                set(evidence) == FULL_EVIDENCE_STAGES,
                f"{name}: full evidence must cover exactly all required stages",
            )
            for stage, proof in evidence.items():
                require(isinstance(proof, dict), f"{name}: evidence for {stage} must be an object")
                require(proof.get("status") == "verified", f"{name}: {stage} is not verified")
                require(
                    isinstance(proof.get("evidence"), str) and proof["evidence"].strip(),
                    f"{name}: {stage} requires a concrete evidence reference",
                )
        else:
            require(evidence is None, f"{name}: completion_evidence is reserved for verified full coverage")

    duplicates = sorted(name for name in set(names) if names.count(name) > 1)
    require(not duplicates, f"duplicate ledger AST variant(s): {', '.join(duplicates)}")
    header_set, ledger_set = set(header_variants), set(names)
    missing = sorted(header_set - ledger_set)
    extra = sorted(ledger_set - header_set)
    require(not missing, f"ledger is missing AST variant(s): {', '.join(missing)}")
    require(not extra, f"ledger has extra AST variant(s): {', '.join(extra)}")
    require(names == header_variants, "ledger AST variants must preserve ASTNodeType order")

    require(len(records) == EXPECTED_VARIANTS, f"ledger must contain exactly {EXPECTED_VARIANTS} records")
    require(counts["full"] == 0, f"current ledger must report 0 full variants; found {counts['full']}")
    require(counts["partial"] == len(EXPECTED_PARTIAL), f"expected exactly {len(EXPECTED_PARTIAL)} partial variants")
    actual_partial = {record["ast_variant"] for record in records if record["lowering_status"] == "partial"}
    require(actual_partial == EXPECTED_PARTIAL, "partial variant set differs from the documented seven")
    listed_partial = ledger.get("partial_variants")
    require(
        isinstance(listed_partial, list)
        and all(isinstance(item, str) for item in listed_partial)
        and len(listed_partial) == len(set(listed_partial))
        and set(listed_partial) == actual_partial,
        "partial_variants must list each partial AST variant exactly once",
    )
    require(counts["not_lowered"] == EXPECTED_VARIANTS - len(EXPECTED_PARTIAL), "not_lowered count invariant failed")
    require(
        ledger.get("coverage_invariants") == counts,
        f"coverage_invariants must equal observed counts {counts}",
    )
    require(
        contract_counts == EXPECTED_CONTRACT_COUNTS,
        f"language-contract classification counts differ from documented inventory: {contract_counts}",
    )
    require(
        ledger.get("classification_invariants") == contract_counts,
        f"classification_invariants must equal observed counts {contract_counts}",
    )
    sources = ledger.get("classification_sources")
    require(isinstance(sources, dict), "ledger classification_sources must be an object")
    for key in ("documented_syntax_contract", "unresolved_public_contract", "unresolved_enum_only_legacy_status", "semantic_status", "testing_status"):
        require(isinstance(sources.get(key), str) and sources[key].strip(), f"missing classification source: {key}")

    return len(records), counts["full"], counts["partial"], counts["not_lowered"]


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ast-header", type=Path, default=root / "include/ast.h")
    parser.add_argument("--ledger", type=Path, default=root / "docs/AST_TYPED_IR_COVERAGE.json")
    args = parser.parse_args()
    try:
        total, full, partial, not_lowered = check_ledger(args.ast_header, args.ledger)
    except (LedgerError, OSError) as exc:
        print(f"AST→typed-IR coverage check failed: {exc}", file=sys.stderr)
        return 1
    print(
        "AST→typed-IR coverage ledger: "
        f"{total} real variants; {full}/{total} full, {partial} partial, "
        f"{not_lowered} not lowered; {SENTINEL} excluded."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
