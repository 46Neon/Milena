#!/usr/bin/env python3
"""Compare canonical grouped CSV streaming with and without local spill.

Generated input is deterministic. Timings are observations, not performance or
RSS guarantees; the spill route remains local and opt-in.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import platform
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "milena"
MAX_ROWS = 1_000_000
RUN_TIMEOUT_SECONDS = 300
MEMORY_BUDGET_BYTES = 262_144
MAX_KEY_BYTES = 128
MAX_REPORT_BYTES = 1_073_741_824
SPILL_QUOTA_MULTIPLIER = 16
MAX_RUNS = 65_536


def write_csv(path: Path, rows: int, groups: int) -> int:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(["grupo", "valor"])
        for i in range(rows):
            writer.writerow([f"G{i % groups:06d}", i % 97])
    return path.stat().st_size


def render_script(csv_path: Path, report_name: str, groups: int,
                  row_limit: int, scratch: Path | None = None,
                  spill_quota: int = 0) -> str:
    spill = ""
    if scratch is not None:
        spill = (f' #spill("{scratch}", {MEMORY_BUDGET_BYTES}, '
                 f'{spill_quota}, {MAX_KEY_BYTES}, {groups}, '
                 f'{MAX_REPORT_BYTES}, {MAX_RUNS})')
    return f''' .analisis benchmark_spill {{
    variable grupo texto
    variable valor numerica
    datos desde "{csv_path}" con grupos de {groups} con filas hasta {row_limit} con tiempo hasta 3600000 ms
    agrupar por "grupo"{spill} resumir {{ suma de "valor"; }}
    guardar resultado en "{report_name}"
}}
'''.lstrip()


def run(binary: Path, script: Path, cwd: Path) -> tuple[dict, float]:
    started = time.perf_counter()
    process = subprocess.run([str(binary), "run", str(script)], cwd=cwd,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=RUN_TIMEOUT_SECONDS)
    elapsed = time.perf_counter() - started
    if process.returncode:
        raise SystemExit(process.stderr or process.stdout or
                         f"Milena exited with status {process.returncode}")
    return json.loads((cwd / script.with_suffix(".json").name).read_text(
        encoding="utf-8")), elapsed


def values(report: dict, expected_mode: str, expected_groups: int) -> dict[str, float | None]:
    if report.get("modo") != expected_mode:
        raise AssertionError(f"expected mode {expected_mode}, got {report.get('modo')}")
    if report.get("grupos") != expected_groups:
        raise AssertionError(f"expected {expected_groups} groups, got {report.get('grupos')}")
    result = {}
    for group in report["resultados"]:
        metric = group["metricas"][0]
        result[group["clave"]] = metric["valor"]
    return result


def assert_row_contract(report: dict, rows: int, groups: int) -> None:
    if report.get("filas") != rows or report.get("filas_validas") != rows:
        raise AssertionError(f"row count mismatch: expected {rows}, got {report.get('filas')} / {report.get('filas_validas')}")
    if report.get("filas_malformadas") != 0 or report.get("limite_grupos") != groups:
        raise AssertionError("malformed-row or configured-group limit mismatch")
    observed = {item["clave"]: item["metricas"][0] for item in report["resultados"]}
    if len(observed) != groups:
        raise AssertionError(f"expected {groups} result groups, got {len(observed)}")
    for index in range(groups):
        key = f"G{index:06d}"
        expected = rows // groups + (1 if index < rows % groups else 0)
        metric = observed.get(key)
        if metric is None or metric.get("valores_validos") != expected:
            raise AssertionError(f"valid-row count mismatch for {key}")
        if metric.get("valores_nulos") != 0 or metric.get("valores_invalidos") != 0:
            raise AssertionError(f"unexpected null/invalid rows for {key}")


def run_case(rows: int, groups: int, repetitions: int) -> dict:
    with tempfile.TemporaryDirectory(prefix="milena-grouped-spill-bench-") as td:
        root = Path(td)
        csv_path = root / "rows.csv"
        byte_count = write_csv(csv_path, rows, groups)
        quota = min(4_294_967_296,
                    max(1_048_576, byte_count * SPILL_QUOTA_MULTIPLIER))
        measurements = []
        for repetition in range(repetitions):
            memory_script = root / f"memory-{repetition}.milena"
            spill_script = root / f"spill-{repetition}.milena"
            memory_script.write_text(render_script(csv_path,
                f"memory-{repetition}.json", groups, rows + 1), encoding="utf-8")
            scratch = root / f"scratch-{repetition}.bin"
            spill_script.write_text(render_script(csv_path,
                f"spill-{repetition}.json", groups, rows + 1,
                scratch=scratch, spill_quota=quota), encoding="utf-8")
            memory_report, memory_seconds = run(BINARY, memory_script, root)
            spill_report, spill_seconds = run(BINARY, spill_script, root)
            assert_row_contract(memory_report, rows, groups)
            assert_row_contract(spill_report, rows, groups)
            memory_values = values(memory_report, "flujo_agrupado", groups)
            spill_values = values(spill_report, "flujo_agrupado_spill", groups)
            if memory_values.keys() != spill_values.keys():
                raise AssertionError("memory and spill group keys differ")
            for key, expected in memory_values.items():
                observed = spill_values[key]
                if expected is None or observed is None:
                    if expected is not observed:
                        raise AssertionError(f"null mismatch for {key}")
                elif not math.isclose(expected, observed,
                                      rel_tol=1e-10, abs_tol=1e-10):
                    raise AssertionError(f"value mismatch for {key}: {expected} != {observed}")
            if scratch.exists():
                raise AssertionError("spill scratch file remains after successful run")
            measurements.append({
                "repetition": repetition + 1,
                "memory_elapsed_seconds": memory_seconds,
                "spill_elapsed_seconds": spill_seconds,
                "memory_backend_elapsed_ms": memory_report.get("tiempo_ms"),
                "spill_backend_elapsed_ms": spill_report.get("tiempo_ms"),
                "spill_report_bytes": spill_report.get("bytes_salida"),
                "spill_limit_bytes": spill_report.get("limite_salida_bytes"),
            })
        return {"rows": rows, "groups": groups, "input_bytes": byte_count,
                "spill_memory_budget_bytes": MEMORY_BUDGET_BYTES,
                "spill_quota_bytes": quota, "max_runs": MAX_RUNS,
                "measurements": measurements}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--large-rows", type=int, default=0,
                        help="opt-in workload up to one million rows")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not BINARY.is_file():
        raise SystemExit("build ./milena first (make all)")
    if not 1 <= args.repetitions <= 20:
        raise SystemExit("--repetitions must be between 1 and 20")
    if not 0 <= args.large_rows <= MAX_ROWS:
        raise SystemExit(f"--large-rows must be between 0 and {MAX_ROWS}")
    cases = [("small", 100, 4), ("medium", 10_000, 32)]
    if args.large_rows:
        cases.append(("large-opt-in", args.large_rows,
                      min(args.large_rows, 1_000)))
    payload = {
        "schema": "milena-grouped-spill-benchmark-v1",
        "workloads": [{"fixture": name, **run_case(rows, groups,
                                                       args.repetitions)}
                      for name, rows, groups in cases],
        "environment": {"platform": platform.platform(),
                        "machine": platform.machine(),
                        "compiler": os.environ.get("CC", "make default CC"),
                        "commit": os.environ.get("GITHUB_SHA", "unknown")},
        "methodology": ("Deterministic generated CSV; canonical lexer-parser-AST-"
                        "semantic-runtime-stream backend; compare in-memory and "
                        "spill output values and record wall-clock observations."),
        "limitations": [
            "No RSS measurement or global memory guarantee; configured reducer memory is not total process RSS.",
            "No SLO or performance guarantee; results apply only to the recorded environment and workload.",
            "No CSV-aware partition execution, Arrow/Parquet, cloud or distributed execution is measured.",
        ],
    }
    rendered = json.dumps(payload, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
