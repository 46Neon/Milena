#!/usr/bin/env python3
"""Measure canonical grouped CSV streaming with deterministic bounded fixtures.

Small and medium workloads run in CI. Larger workloads are explicit opt-in;
measurements are observations, not performance guarantees.
"""
from __future__ import annotations
import argparse
import csv
import json
import os
import platform
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "milena"
MAX_LARGE_ROWS = 1_000_000
RUN_TIMEOUT_SECONDS = 180


def make_csv(path: Path, rows: int, groups: int, malformed_every: int) -> int:
    malformed = 0
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(["grupo", "valor", "referencia"])
        for i in range(rows):
            bad = bool(malformed_every and (i + 1) % malformed_every == 0)
            writer.writerow([f"G{i % groups:04d}",
                             "no-num" if bad else f"{(i % 101) / 10:.1f}",
                             f"r{i:08d}"])
            malformed += int(bad)
    return malformed


def run_case(label: str, rows: int, groups: int,
             malformed_every: int, large: bool) -> dict:
    with tempfile.TemporaryDirectory(prefix="milena-grouped-stream-") as td:
        root = Path(td)
        csv_path = root / "datos.csv"
        script_path = root / "agrupado.milena"
        expected_bad = make_csv(csv_path, rows, groups, malformed_every)
        script_path.write_text(f''' .analisis benchmark_agrupado {{
    datos desde "{csv_path}" procesar por lotes de 4096 filas con grupos de {groups}
    agrupar por "grupo" resumir {{ suma de "valor"; contar de "referencia"; }}
    guardar resultado en "reporte.json"
}}
'''.lstrip(), encoding="utf-8")
        started = time.perf_counter()
        result = subprocess.run([str(BINARY), "run", str(script_path)],
            cwd=ROOT, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=RUN_TIMEOUT_SECONDS)
        elapsed = time.perf_counter() - started
        if result.returncode:
            raise SystemExit(result.stderr or result.stdout)
        report = json.loads((root / "reporte.json").read_text(encoding="utf-8"))
        assert report["modo"] == "flujo_agrupado"
        assert report["filas"] == rows
        assert report["filas_malformadas"] == expected_bad
        assert report["grupos"] == groups
        assert report["limite_grupos"] == groups
        invalid = sum(metric["valores_invalidos"]
                      for group in report["resultados"]
                      for metric in group["metricas"]
                      if metric["operacion"] == "suma")
        assert invalid == expected_bad
        return {"fixture": label, "rows_requested": rows,
                "groups_expected": groups, "expected_invalid_numeric": expected_bad,
                "rows": report["filas"], "rows_valid": report["filas_validas"],
                "rows_malformed": report["filas_malformadas"],
                "groups": report["grupos"], "bytes": report["bytes_entrada"],
                "elapsed_seconds_process": elapsed,
                "backend_elapsed_milliseconds": report["tiempo_ms"],
                "rows_per_second": report["filas_por_segundo"],
                "megabytes_per_second": report["megabytes_por_segundo"],
                "observed_record_bytes": report["pico_registro_bytes"],
                "record_buffer_capacity_bytes": report["capacidad_buffer_registro_bytes"],
                "large_opt_in": large}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--large-rows", type=int, default=0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not BINARY.is_file():
        raise SystemExit("build ./milena first (make all)")
    if args.large_rows < 0:
        raise SystemExit("--large-rows must be non-negative")
    if args.large_rows > MAX_LARGE_ROWS:
        raise SystemExit(f"--large-rows must not exceed {MAX_LARGE_ROWS}")
    cases = [("small", 100, 4, 17), ("medium", 10000, 32, 997)]
    if args.large_rows:
        cases.append(("large-opt-in", args.large_rows,
                      min(args.large_rows, 1024), 997))
    payload = {
        "schema": "milena-grouped-stream-benchmark-v1",
        "workloads": [run_case(name, count, group_count, invalid_every,
                                name.startswith("large"))
                      for name, count, group_count, invalid_every in cases],
        "environment": {"platform": platform.platform(),
                        "machine": platform.machine(),
                        "compiler": os.environ.get("CC", "make default CC"),
                        "commit": os.environ.get("GITHUB_SHA", "unknown")},
        "methodology": "Deterministic generated CSV; canonical lexer-parser-AST-semantic-runtime-stream backend; bounded group and state budgets; wall-clock observations.",
        "limitations": ["No spill-to-disk, CSV-aware partition execution, Arrow/Parquet, cloud, Spark, Flink or distributed execution is measured.",
                        "Results are not a latency or industrial-scale claim."],
    }
    rendered = json.dumps(payload, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
