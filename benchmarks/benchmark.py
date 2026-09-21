#!/usr/bin/env python3
"""Measure a clean Milena build and canonical script execution.

The harness intentionally reports observations only.  It does not set a
performance target and must not be used to claim that every workload is fast.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "benchmarks" / "ejecucion.milena"
BINARY = ROOT / "milena"


def measure(command: list[str], repetitions: int, *, quiet: bool = True) -> list[float]:
    samples: list[float] = []
    for _ in range(repetitions):
        started = time.perf_counter()
        result = subprocess.run(
            command,
            cwd=ROOT,
            stdout=subprocess.DEVNULL if quiet else None,
            stderr=subprocess.PIPE,
            text=True,
        )
        elapsed = time.perf_counter() - started
        if result.returncode:
            sys.stderr.write(result.stderr)
            raise SystemExit(f"command failed ({result.returncode}): {' '.join(command)}")
        samples.append(elapsed)
    return samples


def summary(samples: list[float]) -> dict[str, float | int]:
    return {
        "samples": len(samples),
        "min_seconds": min(samples),
        "median_seconds": statistics.median(samples),
        "mean_seconds": statistics.fmean(samples),
        "max_seconds": max(samples),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-repetitions", type=int, default=1)
    parser.add_argument("--run-repetitions", type=int, default=5)
    parser.add_argument("--output", type=Path, help="write the JSON result to this path")
    args = parser.parse_args()
    if args.compile_repetitions < 1 or args.run_repetitions < 1:
        parser.error("repetition counts must be positive")
    make = shutil.which("make")
    if make is None:
        raise SystemExit("make is required for the compile benchmark")
    if not FIXTURE.is_file():
        raise SystemExit(f"missing benchmark fixture: {FIXTURE}")

    compile_samples: list[float] = []
    for _ in range(args.compile_repetitions):
        subprocess.run([make, "clean"], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        compile_samples.extend(measure([make, "-B", "all"], 1))
    if not BINARY.is_file():
        raise SystemExit("build completed without producing ./milena")
    run_samples = measure([str(BINARY), "run", str(FIXTURE)], args.run_repetitions)

    result = {
        "schema": "milena-benchmark-v1",
        "fixture": str(FIXTURE.relative_to(ROOT)),
        "compile": {"command": "make clean && make -B all", **summary(compile_samples)},
        "execution": {
            "command": "./milena run benchmarks/ejecucion.milena",
            **summary(run_samples),
        },
        "environment": {
            "python": platform.python_version(),
            "platform": platform.platform(),
            "machine": platform.machine(),
            "compiler": os.environ.get("CC", "make default CC"),
            "commit": os.environ.get("GITHUB_SHA", "unknown"),
        },
        "notes": [
            "Wall-clock samples from time.perf_counter; no performance guarantee.",
            "Compile samples start from a clean tree and use the repository Makefile.",
            "Execution uses the canonical lexer-parser-AST-semantic-runtime path.",
        ],
    }
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
