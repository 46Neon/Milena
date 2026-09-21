#!/usr/bin/env python3
"""Verifica que cada fuente C pertenezca a una capa conocida de Milena."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]

CATEGORIES = {
    "producto": {
        "analysis.c", "array.c", "common.c", "dataset.c", "entrypoints.c", "finance.c",
        "language_runtime.c", "logger.c", "main.c", "metrics.c", "schema.c",
        "script.c", "interpreter.c", "sst_advanced.c", "sst_contingency.c", "sst_correlation.c",
        "sst_dates.c", "sst_histogram.c", "sst_inference.c", "sst_model.c",
        "sst_normality.c", "sst_rates.c", "sst_report.c",
        "sst_report_advanced.c", "sst_stats.c", "stream.c", "table.c",
    },
    "lenguaje": {
        "ast.c", "language_semantic.c", "lexer.c", "parser.c", "symbol_table.c",
    },
    "funciones": {
        "function_parser.c", "user_functions.c", "symbol.c",
    },
    "experimental": {
        "arena.c", "assembler.c", "compiler.c", "forest.c", "gc.c",
        "instructions.c", "ir.c", "module.c", "semantic.c",
        "temp_scope.c", "vm.c",
    },
}


def main() -> int:
    source_files = {path.name for path in (ROOT / "src").glob("*.c")}
    owners = {}
    errors = []

    for category, files in CATEGORIES.items():
        for name in files:
            previous = owners.setdefault(name, category)
            if previous != category:
                errors.append(f"{name}: aparece en {previous} y {category}")

    missing = sorted(source_files - set(owners))
    extra = sorted(set(owners) - source_files)
    if missing:
        errors.append("fuentes sin categoría: " + ", ".join(missing))
    if extra:
        errors.append("fuentes declaradas pero inexistentes: " + ", ".join(extra))

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"OK: {len(source_files)} fuentes C clasificadas")
    for category in CATEGORIES:
        print(f"  - {category}: {len(CATEGORIES[category])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
