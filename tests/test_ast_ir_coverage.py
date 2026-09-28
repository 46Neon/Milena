#!/usr/bin/env python3
"""Mutation tests for the AST-to-typed-IR coverage ledger checker."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / "tools" / "check_ast_ir_coverage.py"
LEDGER = ROOT / "docs" / "AST_TYPED_IR_COVERAGE.json"
CONTRACT_DOC = ROOT / "docs" / "COMPILADOR_IR_CONTRATOS_AST.md"
AST_HEADER = ROOT / "include" / "ast.h"


class ASTIRCoverageCheckerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.temp_path = Path(self.temp_dir.name)
        self.ledger = json.loads(LEDGER.read_text(encoding="utf-8"))

    def run_checker(self, ledger: dict[str, Any] | None = None,
                    header_text: str | None = None,
                    contract_doc_text: str | None = None) -> subprocess.CompletedProcess[str]:
        ledger_path = self.temp_path / "ledger.json"
        ledger_path.write_text(
            json.dumps(self.ledger if ledger is None else ledger), encoding="utf-8"
        )
        header_path = AST_HEADER
        if header_text is not None:
            header_path = self.temp_path / "ast.h"
            header_path.write_text(header_text, encoding="utf-8")
        contract_doc_path = CONTRACT_DOC
        if contract_doc_text is not None:
            contract_doc_path = self.temp_path / "contracts.md"
            contract_doc_path.write_text(contract_doc_text, encoding="utf-8")
        return subprocess.run(
            [
                sys.executable,
                str(CHECKER),
                "--ast-header",
                str(header_path),
                "--ledger",
                str(ledger_path),
                "--contract-doc",
                str(contract_doc_path),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_current_ledger_passes(self) -> None:
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "72 enum variants; 0/68 source constructs full, 7 partial, 61 not lowered, 4 reserved",
            result.stdout,
        )

    def test_duplicate_variant_is_rejected(self) -> None:
        ledger = json.loads(json.dumps(self.ledger))
        ledger["variants"].append(dict(ledger["variants"][0]))
        self.assertNotEqual(self.run_checker(ledger).returncode, 0)

    def test_missing_variant_is_rejected(self) -> None:
        ledger = json.loads(json.dumps(self.ledger))
        ledger["variants"].pop()
        self.assertNotEqual(self.run_checker(ledger).returncode, 0)

    def test_extra_variant_is_rejected(self) -> None:
        ledger = json.loads(json.dumps(self.ledger))
        extra = dict(ledger["variants"][0])
        extra["ast_variant"] = "AST_NOT_IN_HEADER"
        ledger["variants"].append(extra)
        self.assertNotEqual(self.run_checker(ledger).returncode, 0)

    def test_sentinel_entry_is_rejected(self) -> None:
        ledger = json.loads(json.dumps(self.ledger))
        ledger["variants"][0]["ast_variant"] = "AST_NODE_TYPE_COUNT"
        self.assertNotEqual(self.run_checker(ledger).returncode, 0)

    def test_unknown_lowering_status_is_rejected(self) -> None:
        ledger = json.loads(json.dumps(self.ledger))
        ledger["variants"][0]["lowering_status"] = "assumed_supported"
        self.assertNotEqual(self.run_checker(ledger).returncode, 0)

    def test_full_without_all_stage_evidence_is_rejected(self) -> None:
        ledger = json.loads(json.dumps(self.ledger))
        ledger["variants"][0]["lowering_status"] = "full"
        self.assertNotEqual(self.run_checker(ledger).returncode, 0)

    def test_contract_classification_count_drift_is_rejected(self) -> None:
        ledger = json.loads(json.dumps(self.ledger))
        record = ledger["variants"][0]
        record["language_contract_status"] = "unresolved_public_contract"
        record["parser_status"] = "parser_reachable_contract_unresolved"
        self.assertNotEqual(self.run_checker(ledger).returncode, 0)

    def test_four_enum_only_variants_are_reserved_outside_language_coverage(self) -> None:
        reserved = {
            "AST_ASIGNACION_DATASET", "AST_BLOQUE_VISUALIZAR",
            "AST_EXPRESION_FUNCION", "AST_COMANDO_EXTRAER",
        }
        records = {record["ast_variant"]: record for record in self.ledger["variants"]}
        self.assertEqual(
            self.ledger["classification_invariants"][
                "reserved_internal_enum_not_language_construct"
            ], 4,
        )
        for name in reserved:
            with self.subTest(ast_variant=name):
                record = records[name]
                self.assertEqual(record["lowering_status"], "not_applicable_reserved")
                self.assertEqual(
                    record["language_contract_status"],
                    "reserved_internal_enum_not_language_construct",
                )
                self.assertEqual(record["semantic_status"], "not_applicable_reserved")
                self.assertEqual(record["testing_status"], "not_applicable_reserved")

    def test_reserved_status_is_rejected_for_source_construct(self) -> None:
        ledger = json.loads(json.dumps(self.ledger))
        record = next(item for item in ledger["variants"]
                      if item["ast_variant"] == "AST_DECLARACION_DATOS")
        record["lowering_status"] = "not_applicable_reserved"
        record["language_contract_status"] = "reserved_internal_enum_not_language_construct"
        record["parser_status"] = "enum_only_reserved_not_language_construct"
        record["semantic_status"] = "not_applicable_reserved"
        record["testing_status"] = "not_applicable_reserved"
        self.assertNotEqual(self.run_checker(ledger).returncode, 0)

    def test_full_with_incomplete_stage_evidence_is_rejected(self) -> None:
        ledger = json.loads(json.dumps(self.ledger))
        record = ledger["variants"][0]
        record["lowering_status"] = "full"
        record["semantic_status"] = "verified"
        record["testing_status"] = "verified"
        record["completion_evidence"] = {
            "language_contract": {"status": "verified", "evidence": "docs/example.md"}
        }
        result = self.run_checker(ledger)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("full evidence must cover exactly all required stages", result.stderr)

    def test_documented_syntax_evidence_is_classified_as_documented(self) -> None:
        required_sources = {
            "AST_COMANDO_SST": ("docs/MILENA_ANALYSIS_LANGUAGE.md",),
            "AST_STREAM_FILTER": ("docs/MILENA_ANALYSIS_LANGUAGE.md",),
            "AST_BLOQUE_UNIR": ("docs/JOIN_RESOURCE_LIMITS.md",),
            "AST_COMANDO_DERECHA": ("docs/JOIN_RESOURCE_LIMITS.md",),
            "AST_COMANDO_CLAVE": ("docs/JOIN_RESOURCE_LIMITS.md",),
        }
        records = {record["ast_variant"]: record for record in self.ledger["variants"]}
        for name, sources in required_sources.items():
            with self.subTest(ast_variant=name):
                record = records[name]
                self.assertEqual(record["language_contract_status"],
                                 "documented_syntax_contract")
                self.assertEqual(record["parser_status"],
                                 "parser_reachable_documented_contract")
                for source in sources:
                    self.assertIn(source, record["inventory_evidence"])
                    self.assertTrue((ROOT / source).is_file())

    def test_all_newly_closed_variants_have_full_contract_sections(self) -> None:
        text = CONTRACT_DOC.read_text(encoding="utf-8")
        expected = {
            "AST_BLOQUE_LIMPIAR", "AST_BLOQUE_TRANSFORMAR", "AST_BLOQUE_FILTRAR",
            "AST_COMANDO_NULOS", "AST_COMANDO_DUPLICADOS", "AST_COMANDO_CONDICION",
            "AST_COMANDO_TOTAL", "AST_COMANDO_PERIODO", "AST_DECLARACION_ENTRADA",
            "AST_DECLARACION_SALIDA", "AST_BLOQUE_SELECCIONAR", "AST_COMANDO_COLUMNAS",
            "AST_COLUMNAR_PROJECT", "AST_COLUMNAR_FIELD", "AST_DECLARACION_DATOS",
            "AST_DECLARACION_ESTADISTICA",
        }
        for name in expected:
            with self.subTest(ast_variant=name):
                self.assertIn(f"### `{name}`", text)
                record = next(item for item in self.ledger["variants"]
                              if item["ast_variant"] == name)
                self.assertEqual(record["language_contract_status"],
                                 "documented_syntax_contract")
                self.assertEqual(record["parser_status"],
                                 "parser_reachable_documented_contract")
                self.assertIn("docs/COMPILADOR_IR_CONTRATOS_AST.md",
                              record["inventory_evidence"])

    def test_missing_normative_contract_section_is_rejected(self) -> None:
        text = CONTRACT_DOC.read_text(encoding="utf-8")
        text = text.replace("### `AST_COMANDO_PERIODO`", "### `AST_COMMAND_PERIOD_REMOVED`", 1)
        result = self.run_checker(contract_doc_text=text)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing variant contract(s)", result.stderr)

    def test_incomplete_normative_contract_fields_are_rejected(self) -> None:
        text = CONTRACT_DOC.read_text(encoding="utf-8")
        text = text.replace("- **Recursos:**", "- Recursos:", 1)
        result = self.run_checker(contract_doc_text=text)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("normative contract is missing field(s)", result.stderr)

    def test_duplicate_enum_member_is_rejected(self) -> None:
        header = AST_HEADER.read_text(encoding="utf-8")
        header = header.replace("    AST_BLOQUE_ANALISIS,", "    AST_PROGRAMA,", 1)
        self.assertNotEqual(self.run_checker(header_text=header).returncode, 0)


if __name__ == "__main__":
    unittest.main()
