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
AST_HEADER = ROOT / "include" / "ast.h"


class ASTIRCoverageCheckerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.temp_path = Path(self.temp_dir.name)
        self.ledger = json.loads(LEDGER.read_text(encoding="utf-8"))

    def run_checker(self, ledger: dict[str, Any] | None = None,
                    header_text: str | None = None) -> subprocess.CompletedProcess[str]:
        ledger_path = self.temp_path / "ledger.json"
        ledger_path.write_text(
            json.dumps(self.ledger if ledger is None else ledger), encoding="utf-8"
        )
        header_path = AST_HEADER
        if header_text is not None:
            header_path = self.temp_path / "ast.h"
            header_path.write_text(header_text, encoding="utf-8")
        return subprocess.run(
            [
                sys.executable,
                str(CHECKER),
                "--ast-header",
                str(header_path),
                "--ledger",
                str(ledger_path),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_current_ledger_passes(self) -> None:
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("72 real variants; 0/72 full, 7 partial, 65 not lowered", result.stdout)

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

    def test_duplicate_enum_member_is_rejected(self) -> None:
        header = AST_HEADER.read_text(encoding="utf-8")
        header = header.replace("    AST_BLOQUE_ANALISIS,", "    AST_PROGRAMA,", 1)
        self.assertNotEqual(self.run_checker(header_text=header).returncode, 0)


if __name__ == "__main__":
    unittest.main()
