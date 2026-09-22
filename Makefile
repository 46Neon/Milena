#!/usr/bin/env python3
"""Measure the canonical Spanish streaming path on deterministic CSV fixtures.

The script generates fixtures, writes a human-language .milena program, and
invokes ``milena run``. It reports observations only; it sets no performance
claim or industrial target. Large workloads are opt-in.
"""
from __future__ import annotations
import argparse, csv, json, os, platform, subprocess, tempfile, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "milena"

def make_csv(path: Path, rows: int, malformed_every: int) -> int:
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["id", "importe", "grupo"])
        malformed = 0
        for i in range(rows):
            if malformed_every and (i + 1) % malformed_every == 0:
                w.writerow([i, "no-num", "invalida"]); malformed += 1
            else:
                w.writerow([i, f"{(i % 1000) / 10:.1f}", "A" if i % 2 else "B"])
    return malformed

def one(label: str, rows: int, malformed_every: int, large: bool) -> dict:
    with tempfile.TemporaryDirectory(prefix="milena-stream-") as td:
        root = Path(td); csv_path = root / "datos.csv"; script = root / "flujo.milena"
        expected_bad = make_csv(csv_path, rows, malformed_every)
        script.write_text(f'''.analisis benchmark {{\n    datos desde "{csv_path}"\n        procesar por lotes de 4096 filas\n        con registros de hasta 1 MiB\n        con columnas de 16\n    resumir {{ suma de "importe"; media de "importe"; contar de "importe"; }}\n    guardar resultado en "reporte.json"\n}}\n''', encoding="utf-8")
        started = time.perf_counter()
        result = subprocess.run([str(BINARY), "run", str(script)], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        elapsed = time.perf_counter() - started
        if result.returncode:
            raise SystemExit(result.stderr or result.stdout)
        report = json.loads((root / "reporte.json").read_text(encoding="utf-8"))
        assert report["filas"] == rows and report["filas_malformadas"] == expected_bad
        return {"fixture": label, "rows_requested": rows, "expected_malformed": expected_bad,
                "rows": report["filas"], "rows_valid": report["filas_validas"],
                "rows_malformed": report["filas_malformadas"], "bytes": report["bytes_entrada"],
                "elapsed_seconds_process": elapsed, "rows_per_second": report["filas_por_segundo"],
                "megabytes_per_second": report["megabytes_por_segundo"],
                "observed_record_buffer_bytes": report["pico_registro_bytes"],
                "configured_record_limit_bytes": report["limite_registro_bytes"],
                "chunk_rows": report["tamano_lote"], "large_opt_in": large}

def main() -> int:
    p = argparse.ArgumentParser(); p.add_argument("--large-rows", type=int, default=0)
    p.add_argument("--output", type=Path); args = p.parse_args()
    if not BINARY.is_file(): raise SystemExit("build ./milena first (make all)")
    cases = [("small", 100, 17), ("medium", 10000, 997)]
    if args.large_rows:
        if args.large_rows < 1: raise SystemExit("--large-rows must be positive")
        cases.append(("large-opt-in", args.large_rows, 100003))
    result = {"schema": "milena-stream-benchmark-v1", "workloads": [one(n, r, m, n.startswith("large")) for n, r, m in cases],
              "environment": {"platform": platform.platform(), "machine": platform.machine(),
                              "compiler": os.environ.get("CC", "make default CC"),
                              "commit": os.environ.get("GITHUB_SHA", "unknown")},
              "methodology": "Deterministic generated CSV; canonical lexer-parser-AST-semantic-runtime-backend; wall-clock observations; large is opt-in.",
              "limitations": ["No grouping, spill-to-disk, Arrow/Parquet or parallel execution is measured.", "Results are not an industrial-scale claim."]}
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output: args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end=""); return 0
if __name__ == "__main__": raise SystemExit(main())
