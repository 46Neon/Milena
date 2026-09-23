#!/usr/bin/env python3
"""Validate one million rows through Milena's canonical language/runtime path.

Only fixture generation and result checking happen in Python. The CSV scan and
aggregation are executed by ``milena run`` using the human .analisis syntax.
Timings and RSS are observations, not portable performance promises.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import platform
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "milena"
ROWS = 1_000_000
MALFORMED_EVERY = 100_003
CHUNK_ROWS = 4_096
MAX_RECORD_BYTES = 1 * 1024 * 1024
RUN_TIMEOUT_SECONDS = 600


def generate_csv(path: Path) -> tuple[int, int]:
    """Write a stable three-column fixture; return malformed rows and bytes."""
    malformed = 0
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(["id", "importe", "grupo"])
        for row in range(ROWS):
            if (row + 1) % MALFORMED_EVERY == 0:
                writer.writerow([row, "no-num", "invalida"])
                malformed += 1
            else:
                writer.writerow([row, f"{(row % 1000) / 10:.1f}", "A" if row % 2 else "B"])
    return malformed, path.stat().st_size


def expected_ticks() -> int:
    """Exact integer-tenths sum, independent of floating-point accumulation."""
    return sum(row % 1000 for row in range(ROWS)
               if (row + 1) % MALFORMED_EVERY != 0)


def peak_child_rss_bytes() -> int | None:
    """Read the child-process peak RSS when Python's resource API supports it."""
    try:
        import resource
        value = int(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
    except (ImportError, AttributeError, OSError, ValueError):
        return None
    if value <= 0:
        return None
    # POSIX leaves units platform-specific: Darwin reports bytes, Linux/BSD
    # commonly report KiB. This workflow runs on Ubuntu; retain a portable label.
    if sys.platform == "darwin":
        normalized = value
    else:
        normalized = value * 1024
    # ru_maxrss is a high-water mark across children; this script launches only
    # the Milena success and row-budget validation processes being measured.
    return normalized


def validate_report(report: dict[str, Any], malformed_expected: int,
                    input_bytes: int) -> dict[str, Any]:
    expected_valid = ROWS - malformed_expected
    if report.get("modo") != "flujo":
        raise AssertionError(f"expected global stream mode, got {report.get('modo')!r}")
    exact_fields = {
        "filas": ROWS,
        "filas_validas": expected_valid,
        "filas_malformadas": malformed_expected,
        "bytes_entrada": input_bytes,
        "bytes_leidos": input_bytes,
        "tamano_lote": CHUNK_ROWS,
        "limite_registro_bytes": MAX_RECORD_BYTES,
    }
    for field, expected in exact_fields.items():
        actual = report.get(field)
        if actual != expected:
            raise AssertionError(f"{field}: expected {expected}, got {actual!r}")

    record_peak = report.get("pico_registro_bytes")
    buffer_capacity = report.get("capacidad_buffer_registro_bytes")
    if not isinstance(record_peak, int) or not isinstance(buffer_capacity, int):
        raise AssertionError("stream report omitted record/buffer observations")
    if not (0 < record_peak <= buffer_capacity <= MAX_RECORD_BYTES):
        raise AssertionError("observed CSV record buffer violates configured bound")
    if buffer_capacity >= input_bytes:
        raise AssertionError("record buffer is not smaller than the complete CSV input")

    results = report.get("resultados")
    if not isinstance(results, list):
        raise AssertionError("stream report omitted aggregate results")
    by_operation = {item.get("operacion"): item for item in results
                    if isinstance(item, dict)}
    expected_operations = {"suma", "media", "conteo"}
    if set(by_operation) != expected_operations:
        raise AssertionError(f"unexpected aggregate operations: {sorted(by_operation)}")

    valid_count = expected_valid
    sum_value = expected_ticks() / 10.0
    mean_value = sum_value / valid_count
    expected_values = {"suma": sum_value, "media": mean_value,
                       "conteo": float(valid_count)}
    for operation, expected in expected_values.items():
        metric = by_operation[operation]
        if metric.get("columna") != "importe":
            raise AssertionError(f"{operation} used an unexpected input column")
        if metric.get("valores_validos") != valid_count:
            raise AssertionError(f"{operation} valid count is incorrect")
        if metric.get("valores_invalidos") != malformed_expected:
            raise AssertionError(f"{operation} malformed-value count is incorrect")
        actual = metric.get("valor")
        if not isinstance(actual, (int, float)) or not math.isfinite(actual):
            raise AssertionError(f"{operation} returned a non-finite result")
        # The input decimals are binary64; assert the exact mathematical result
        # within a tight representation tolerance, while row/error counts are exact.
        if not math.isclose(float(actual), expected, rel_tol=1e-12, abs_tol=1e-9):
            raise AssertionError(f"{operation}: expected {expected:.17g}, got {actual!r}")

    return {
        "rows": ROWS,
        "valid_rows": expected_valid,
        "malformed_rows": malformed_expected,
        "bytes": input_bytes,
        "aggregates": {name: by_operation[name]["valor"]
                       for name in sorted(expected_operations)},
        "observed_record_bytes": record_peak,
        "record_buffer_capacity_bytes": buffer_capacity,
        "configured_record_limit_bytes": MAX_RECORD_BYTES,
        "chunk_rows": CHUNK_ROWS,
        "backend_elapsed_milliseconds": report.get("tiempo_ms"),
        "backend_rows_per_second": report.get("filas_por_segundo"),
        "backend_megabytes_per_second": report.get("megabytes_por_segundo"),
    }


def run_validation(output_path: Path | None) -> dict[str, Any]:
    if not BINARY.is_file():
        raise SystemExit("build the canonical product first (make all)")
    with tempfile.TemporaryDirectory(prefix="milena-million-row-") as tmp:
        work = Path(tmp)
        csv_path = work / "datos_millon.csv"
        script_path = work / "resumen_millon.milena"
        report_path = work / "reporte.json"
        generation_started = time.perf_counter()
        malformed, input_bytes = generate_csv(csv_path)
        generation_seconds = time.perf_counter() - generation_started
        csv_literal = json.dumps(str(csv_path), ensure_ascii=False)
        report_literal = json.dumps(str(report_path), ensure_ascii=False)
        script_path.write_text(
            f'''.analisis validacion_millon_filas {{\n    datos desde {csv_literal}\n        procesar por lotes de {CHUNK_ROWS} filas\n        con registros de hasta 1 MiB\n        con columnas de 16\n        con filas hasta {ROWS}\n    resumir {{\n        suma de "importe";\n        media de "importe";\n        contar de "importe";\n    }}\n    guardar resultado en {report_literal}\n}}\n''',
            encoding="utf-8")

        started = time.perf_counter()
        completed = subprocess.run(
            [str(BINARY), "run", str(script_path)], cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            timeout=RUN_TIMEOUT_SECONDS, check=False)
        process_seconds = time.perf_counter() - started
        if completed.returncode != 0:
            raise RuntimeError(
                f"milena run failed with exit {completed.returncode}:\n"
                f"{completed.stderr}\n{completed.stdout}")
        if not report_path.is_file():
            raise AssertionError("milena run succeeded without writing its report")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        validated = validate_report(report, malformed, input_bytes)

        # Exercise the AST-carried row budget too: a run capped one row below
        # the fixture must fail and must not publish a successful partial report.
        limited_report = work / "reporte_limite.json"
        limited_script = work / "limite.milena"
        limited_script.write_text(
            f'''.analisis validacion_limite_filas {{\n    datos desde {csv_literal}\n        procesar por lotes de {CHUNK_ROWS} filas\n        con registros de hasta 1 MiB\n        con columnas de 16\n        con filas hasta {ROWS - 1}\n    resumir {{ suma de "importe"; }}\n    guardar resultado en {json.dumps(str(limited_report), ensure_ascii=False)}\n}}\n''',
            encoding="utf-8")
        limit_started = time.perf_counter()
        limited = subprocess.run(
            [str(BINARY), "run", str(limited_script)], cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            timeout=RUN_TIMEOUT_SECONDS, check=False)
        limit_seconds = time.perf_counter() - limit_started
        if limited.returncode == 0 or limited_report.exists():
            raise AssertionError("row-limit overflow must fail without a partial success report")

        rss_bytes = peak_child_rss_bytes()
        result: dict[str, Any] = {
            "schema": "milena-million-row-validation-v1",
            "status": "passed",
            "execution": "milena run with .analisis / datos desde / resumir / guardar resultado",
            "canonical_path": "lexer -> parser -> AST -> semantic/resources -> runtime -> stream backend",
            "fixture": {"rows_requested": ROWS,
                        "malformed_every": MALFORMED_EVERY,
                        "malformed_expected": malformed,
                        "valid_expected": ROWS - malformed,
                        "generation_seconds": generation_seconds},
            "resource_limit_check": {"row_limit": ROWS - 1,
                                     "rejected_without_partial_report": True,
                                     "process_elapsed_seconds": limit_seconds},
            "measurements": {**validated,
                             "process_elapsed_seconds": process_seconds,
                             "process_rows_per_second": ROWS / process_seconds if process_seconds else None,
                             "peak_rss_bytes": rss_bytes,
                             "peak_rss_supported": rss_bytes is not None},
            "environment": {"platform": platform.platform(),
                            "machine": platform.machine(),
                            "python": platform.python_version(),
                            "compiler": os.environ.get("CC", "make default CC"),
                            "commit": os.environ.get("GITHUB_SHA", "unknown")},
            "limitations": [
                "This pass proves only this global CSV aggregation, input, build and hardware.",
                "It does not prove arbitrary Big Data workloads, grouped spill, joins, ETL, distributed/cloud execution, Arrow/Parquet or ML.",
                "Throughput and peak RSS are observations; no universal latency or RSS threshold is asserted.",
            ],
        }
        rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
        if output_path:
            output_path.write_text(rendered, encoding="utf-8")
        print(rendered, end="")
        return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        help="also save the JSON observation to this path")
    args = parser.parse_args()
    run_validation(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
