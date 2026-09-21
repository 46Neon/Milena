#!/usr/bin/env python3
"""Deterministic bounded grouped-stream benchmark (small/medium; large opt-in)."""
from __future__ import annotations
import argparse, csv, json, os, platform, subprocess, tempfile, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "milena"

def make_csv(path: Path, rows: int, malformed_every: int, groups: int) -> int:
    bad = 0
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n"); w.writerow(["id", "importe", "grupo"])
        for i in range(rows):
            if malformed_every and (i + 1) % malformed_every == 0:
                w.writerow([i, "no-num", f"G{i % groups}"]); bad += 1
            else: w.writerow([i, f"{(i % 1000) / 10:.1f}", f"G{i % groups}"])
    return bad

def run_case(label: str, rows: int, malformed_every: int, groups: int, large: bool) -> dict:
    with tempfile.TemporaryDirectory(prefix="milena-grouped-stream-") as td:
        root = Path(td); csv_path = root / "datos.csv"; output = root / "reporte.json"; expected_bad = make_csv(csv_path, rows, malformed_every, groups)
        script = root / "flujo.milena"
        script.write_text(f'''dataset cargar flujo("{csv_path}", 4096)\n.agrupar {{ #por("grupo") #suma("importe") #media("importe") #conteo("importe") }}\n.exportar {{ ("{output}") }}\n''', encoding="utf-8")
        start = time.perf_counter(); p = subprocess.run([str(BINARY), "run", str(script)], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE); elapsed = time.perf_counter() - start
        if p.returncode: raise SystemExit(p.stderr or p.stdout)
        report = json.loads(output.read_text(encoding="utf-8")); assert report["grupos"] == groups and report["filas"] == rows and report["limite_grupos"] == 1000
        invalid = sum(m["valores_invalidos"] for r in report["resultados"] for m in r["metricas"])
        assert invalid == expected_bad
        return {"fixture": label, "rows_requested": rows, "groups_expected": groups, "groups": report["grupos"], "rows": report["filas"], "expected_malformed": expected_bad, "invalid_metric_values": invalid, "elapsed_seconds_process": elapsed, "group_cap": report["limite_grupos"], "large_opt_in": large}

def main() -> int:
    p = argparse.ArgumentParser(); p.add_argument("--large-rows", type=int, default=0); p.add_argument("--output", type=Path); a = p.parse_args()
    if not BINARY.is_file(): raise SystemExit("build ./milena first (make all)")
    if a.large_rows < 0: raise SystemExit("--large-rows must be non-negative")
    cases = [("small", 100, 17, 4, False), ("medium", 10000, 997, 32, False)]
    if a.large_rows: cases.append(("large-opt-in", a.large_rows, 100003, min(256, a.large_rows), True))
    result = {"schema": "milena-grouped-stream-benchmark-v1", "workloads": [run_case(*c) for c in cases], "environment": {"platform": platform.platform(), "machine": platform.machine(), "compiler": os.environ.get("CC", "make default CC"), "commit": os.environ.get("GITHUB_SHA", "unknown")}, "methodology": "Deterministic generated CSV; canonical lexer-parser-AST-semantic-runtime-stream backend; bounded group state; wall-clock observations; large is opt-in.", "limitations": ["Single-process in-memory group state; no spill-to-disk, columnar format, parallelism, or distribution.", "Results are observations, not an industrial-scale claim."]}
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if a.output: a.output.write_text(text, encoding="utf-8")
    print(text, end=""); return 0
if __name__ == "__main__": raise SystemExit(main())
