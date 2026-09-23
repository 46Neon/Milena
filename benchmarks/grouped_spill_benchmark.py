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
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("MILENA_BIN", str(ROOT / "milena"))).resolve()
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


def _milena_path(path: Path | str) -> str:
    # Milena accepts forward slashes on Windows; avoid treating path separators
    # as string escapes in the canonical language source.
    return str(path).replace("\\", "/")


def render_script(csv_path: Path, report_name: str, groups: int,
                  row_limit: int, scratch: Path | None = None,
                  spill_quota: int = 0) -> str:
    spill = ""
    if scratch is not None:
        spill = (f' #spill("{_milena_path(scratch)}", {MEMORY_BUDGET_BYTES}, '
                 f'{spill_quota}, {MAX_KEY_BYTES}, {groups}, '
                 f'{MAX_REPORT_BYTES}, {MAX_RUNS})')
    return f''' .analisis benchmark_spill {{
    variable grupo texto
    variable valor numerica
    datos desde "{_milena_path(csv_path)}" con grupos de {groups} con filas hasta {row_limit} con tiempo hasta 3600000 ms
    agrupar por "grupo"{spill} resumir {{ suma de "valor"; }}
    guardar resultado en "{_milena_path(report_name)}"
}}
'''.lstrip()


_MEASURE_CHILD = r"""
import ctypes, json, platform, subprocess, sys, threading, time
try:
    import resource
except ImportError:
    resource = None
command = json.loads(sys.argv[1])
started = time.perf_counter()
process = subprocess.Popen(command, text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
windows_peak = [None]
stop = threading.Event()
def sample_windows_rss():
    if platform.system() != "Windows": return
    class ProcessMemoryCountersEx(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
            ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
            ("PrivateUsage", ctypes.c_size_t)]
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p,
        ctypes.POINTER(ProcessMemoryCountersEx), ctypes.c_ulong]
    psapi.GetProcessMemoryInfo.restype = ctypes.c_int
    handle = kernel32.OpenProcess(0x0400 | 0x0010, 0, process.pid)
    if not handle: return
    try:
        while not stop.is_set():
            counters = ProcessMemoryCountersEx()
            counters.cb = ctypes.sizeof(counters)
            if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                windows_peak[0] = max(windows_peak[0] or 0,
                                      int(counters.PeakWorkingSetSize))
            time.sleep(0.01)
        counters = ProcessMemoryCountersEx()
        counters.cb = ctypes.sizeof(counters)
        if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            windows_peak[0] = max(windows_peak[0] or 0,
                                  int(counters.PeakWorkingSetSize))
    finally:
        kernel32.CloseHandle(handle)
monitor = threading.Thread(target=sample_windows_rss, daemon=True)
monitor.start()
try:
    stdout, stderr = process.communicate(timeout=int(sys.argv[2]))
    elapsed = time.perf_counter() - started
    stop.set(); monitor.join()
    try:
        if platform.system() == "Windows":
            peak = windows_peak[0]
        elif resource is None:
            peak = None
        else:
            peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
            if platform.system() == "Linux": peak = int(peak * 1024)
            elif platform.system() == "Darwin": peak = int(peak)
            else: peak = None
    except (AttributeError, OSError):
        peak = windows_peak[0]
    print(json.dumps({"returncode": process.returncode, "stdout": stdout,
        "stderr": stderr, "elapsed": elapsed, "peak_rss_bytes": peak}))
except subprocess.TimeoutExpired as error:
    process.kill()
    stdout, stderr = process.communicate()
    stop.set(); monitor.join()
    print(json.dumps({"timeout": True, "stdout": stdout or error.stdout or "",
        "stderr": stderr or error.stderr or ""}))
    sys.exit(124)
"""


def _scratch_size(scratch: Path | None) -> int:
    if scratch is None:
        return 0
    total = 0
    for candidate in scratch.parent.glob(scratch.name + "*"):
        try:
            if candidate.is_file():
                total += candidate.stat().st_size
        except OSError:
            continue
    return total


def run(binary: Path, script: Path, cwd: Path,
        scratch: Path | None = None,
        timeout_seconds: int = RUN_TIMEOUT_SECONDS) -> tuple[dict, float, int | None, int]:
    command = [str(binary), "run", str(script)]
    stop = threading.Event()
    observed_scratch = [0]

    def sample_scratch() -> None:
        while not stop.wait(0.01):
            observed_scratch[0] = max(observed_scratch[0],
                                      _scratch_size(scratch))

    sampler = threading.Thread(target=sample_scratch, daemon=True)
    sampler.start()
    started = time.perf_counter()
    try:
        wrapper = subprocess.run(
            [sys.executable, "-c", _MEASURE_CHILD,
             json.dumps(command), str(timeout_seconds)],
            cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout_seconds + 15)
    finally:
        stop.set()
        sampler.join()
        observed_scratch[0] = max(observed_scratch[0], _scratch_size(scratch))
    elapsed_wrapper = time.perf_counter() - started
    try:
        measured = json.loads(wrapper.stdout)
    except json.JSONDecodeError as error:
        raise SystemExit(wrapper.stderr or wrapper.stdout or
                         "Benchmark child runner returned invalid JSON") from error
    if measured.get("timeout"):
        raise SystemExit("Milena benchmark run timed out")
    if wrapper.returncode or measured.get("returncode"):
        raise SystemExit(measured.get("stderr") or measured.get("stdout") or
                         wrapper.stderr or
                         f"Milena exited with status {measured.get('returncode')}")
    report_path = cwd / script.with_suffix(".json").name
    report = json.loads(report_path.read_text(encoding="utf-8"))
    return (report, float(measured.get("elapsed", elapsed_wrapper)),
            measured.get("peak_rss_bytes"), observed_scratch[0])


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


def assert_full_input_read(report: dict, expected_bytes: int, route: str) -> None:
    if report.get("bytes_entrada") != expected_bytes:
        raise AssertionError(
            f"{route} reports {report.get('bytes_entrada')} input bytes; "
            f"expected {expected_bytes}")
    if report.get("bytes_leidos") != expected_bytes:
        raise AssertionError(
            f"{route} consumed {report.get('bytes_leidos')} bytes; "
            f"expected the full {expected_bytes}-byte CSV")


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
            memory_report, memory_seconds, memory_rss, _ = run(
                BINARY, memory_script, root)
            spill_report, spill_seconds, spill_rss, scratch_peak = run(
                BINARY, spill_script, root, scratch=scratch)
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
            spill_counters = {
                "source_spill_bytes": spill_report.get("bytes_spill"),
                "source_spill_records": spill_report.get("registros_spill"),
                "initial_sorted_runs": spill_report.get("runs_spill"),
            }
            if any(not isinstance(value, int) or value < 0
                   for value in spill_counters.values()):
                raise AssertionError("spill report omitted reducer telemetry")
            measurements.append({
                "repetition": repetition + 1,
                "memory_elapsed_seconds": memory_seconds,
                "spill_elapsed_seconds": spill_seconds,
                "memory_backend_elapsed_ms": memory_report.get("tiempo_ms"),
                "spill_backend_elapsed_ms": spill_report.get("tiempo_ms"),
                "spill_report_bytes": spill_report.get("bytes_salida"),
                "spill_limit_bytes": spill_report.get("limite_salida_bytes"),
                "memory_peak_rss_bytes": memory_rss,
                "spill_peak_rss_bytes": spill_rss,
                "spill_scratch_peak_bytes_sampled": scratch_peak,
                **spill_counters,
            })
        return {"rows": rows, "groups": groups, "input_bytes": byte_count,
                "spill_memory_budget_bytes": MEMORY_BUDGET_BYTES,
                "spill_quota_bytes": quota, "max_runs": MAX_RUNS,
                "measurements": measurements}


def _allocated_size(path: Path, file_stat: os.stat_result) -> tuple[int, str]:
    if hasattr(file_stat, "st_blocks") and file_stat.st_blocks > 0:
        return file_stat.st_blocks * 512, "stat.st_blocks * 512"
    if os.name == "nt":
        import ctypes

        high = ctypes.c_ulong(0)
        get_size = ctypes.WinDLL("kernel32", use_last_error=True).GetCompressedFileSizeW
        get_size.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
        get_size.restype = ctypes.c_ulong
        ctypes.set_last_error(0)
        low = get_size(str(path), ctypes.byref(high))
        error = ctypes.get_last_error()
        if low == 0xFFFFFFFF and error:
            raise OSError(error, "GetCompressedFileSizeW failed", str(path))
        return (high.value << 32) | low, "GetCompressedFileSizeW"
    raise OSError(f"cannot measure allocated disk size for {path}")


def write_padded_csv(path: Path, rows: int, groups: int,
                     minimum_bytes: int) -> tuple[int, int, int, str]:
    if rows < 1 or groups != 1_000 or groups > rows:
        raise ValueError("10 GB spill gate requires exactly 1,000 groups and valid rows")
    header = b"grupo,valor,relleno\n"
    first_prefix = b"G000000,1,"
    base_record_bytes = len(first_prefix) + 1  # plus line feed
    wanted_per_row = max(base_record_bytes,
                         (max(0, minimum_bytes - len(header)) + rows - 1) // rows)
    padding_bytes = wanted_per_row - base_record_bytes
    filler = b"x" * padding_bytes
    cycle = b"".join(
        f"G{group:06d},1,".encode("ascii") + filler + b"\n"
        for group in range(groups))
    full_cycles, remainder = divmod(rows, groups)
    cycles_per_chunk = max(1, min(256, (8 * 1024 * 1024) // len(cycle)))
    with path.open("wb") as stream:
        stream.write(header)
        for start in range(0, full_cycles, cycles_per_chunk):
            count = min(cycles_per_chunk, full_cycles - start)
            stream.write(cycle * count)
        for group in range(remainder):
            stream.write(f"G{group:06d},1,".encode("ascii"))
            stream.write(filler)
            stream.write(b"\n")
        stream.flush()
        os.fsync(stream.fileno())
    file_stat = path.stat()
    actual_bytes = file_stat.st_size
    allocated_bytes, allocation_method = _allocated_size(path, file_stat)
    if actual_bytes < minimum_bytes:
        raise AssertionError(
            f"generated CSV is only {actual_bytes} bytes; need {minimum_bytes}")
    if allocated_bytes < minimum_bytes * 0.9:
        raise AssertionError(
            f"CSV allocation looks sparse: {allocated_bytes} allocated bytes "
            f"for {actual_bytes} logical bytes")
    return actual_bytes, padding_bytes, allocated_bytes, allocation_method


def run_large_file_case(minimum_bytes: int, rows: int, groups: int,
                        repetitions: int) -> dict:
    if minimum_bytes < 10_000_000_000:
        raise ValueError("the large-file gate requires at least 10,000,000,000 bytes")
    with tempfile.TemporaryDirectory(prefix="milena-grouped-spill-10gb-") as td:
        root = Path(td)
        free_before = shutil.disk_usage(root).free
        if free_before < minimum_bytes + 512 * 1024 * 1024:
            raise SystemExit(
                f"need at least {minimum_bytes + 512 * 1024 * 1024} free bytes; "
                f"found {free_before}")
        csv_path = root / "ten-gib.csv"
        input_bytes, padding_bytes, allocated_bytes, allocation_method = write_padded_csv(
            csv_path, rows, groups, minimum_bytes)
        free_after_generation = shutil.disk_usage(root).free
        quota = min(4_294_967_296,
                    max(1_048_576, input_bytes * SPILL_QUOTA_MULTIPLIER))
        measurements = []
        for repetition in range(repetitions):
            memory_script = root / f"large-memory-{repetition}.milena"
            spill_script = root / f"large-spill-{repetition}.milena"
            memory_text = render_script(csv_path,
                f"large-memory-{repetition}.json", groups, rows + 1)
            spill_path = root / f"large-scratch-{repetition}.bin"
            spill_text = render_script(csv_path,
                f"large-spill-{repetition}.json", groups, rows + 1,
                scratch=spill_path, spill_quota=quota)
            declaration = "    variable valor numerica\n"
            if declaration not in memory_text or declaration not in spill_text:
                raise AssertionError("cannot add the padding column to the Milena script")
            memory_script.write_text(memory_text.replace(
                declaration, declaration + "    variable relleno texto\n"),
                encoding="utf-8")
            spill_script.write_text(spill_text.replace(
                declaration, declaration + "    variable relleno texto\n"),
                encoding="utf-8")
            memory_report, memory_seconds, memory_rss, _ = run(
                BINARY, memory_script, root, timeout_seconds=2400)
            spill_report, spill_seconds, spill_rss, scratch_peak = run(
                BINARY, spill_script, root, scratch=spill_path,
                timeout_seconds=2400)
            assert_row_contract(memory_report, rows, groups)
            assert_row_contract(spill_report, rows, groups)
            assert_full_input_read(memory_report, input_bytes, "in-memory route")
            assert_full_input_read(spill_report, input_bytes, "spill route")
            memory_values = values(memory_report, "flujo_agrupado", groups)
            spill_values = values(spill_report, "flujo_agrupado_spill", groups)
            if memory_values != spill_values:
                raise AssertionError("large-file memory/spill outputs differ")
            if scratch_peak <= 0:
                raise AssertionError(
                    "large-file spill run produced no sampled scratch bytes; "
                    "the test did not prove an actual disk spill")
            if _scratch_size(spill_path) != 0:
                raise AssertionError("large-file spill scratch files remain after success")
            measurements.append({
                "repetition": repetition + 1,
                "memory_elapsed_seconds": memory_seconds,
                "spill_elapsed_seconds": spill_seconds,
                "memory_peak_rss_bytes": memory_rss,
                "spill_peak_rss_bytes": spill_rss,
                "spill_scratch_peak_bytes_sampled": scratch_peak,
                "input_bytes_read_memory": memory_report.get("bytes_leidos"),
                "input_bytes_read_spill": spill_report.get("bytes_leidos"),
                "spill_output_bytes": spill_report.get("bytes_salida"),
            })
        return {
            "minimum_file_bytes": minimum_bytes,
            "actual_file_bytes": input_bytes,
            "allocated_file_bytes": allocated_bytes,
            "allocation_measurement": allocation_method,
            "rows": rows,
            "groups": groups,
            "padding_bytes_per_row": padding_bytes,
            "spill_memory_budget_bytes": MEMORY_BUDGET_BYTES,
            "spill_quota_bytes": quota,
            "free_disk_bytes_before": free_before,
            "free_disk_bytes_after_generation": free_after_generation,
            "measurements": measurements,
            "limitations": [
                "Synthetic deterministic CSV with a repeated padding column, one million rows and exactly 1,000 groups; the configured 262,144-byte reducer budget forces flushes, but this does not prove arbitrary high-cardinality, global-RSS, or distributed scale.",
                "RSS is per-process peak RSS, not global/cgroup RSS or a system memory cap; sampled scratch is logical spill-file bytes on disk, not RAM.",
            ],
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--large-rows", type=int, default=0,
                        help="opt-in workload up to one million rows")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--large-file-bytes", type=int, default=0,
                        help="also validate a generated CSV of at least 10 GB")
    parser.add_argument("--large-file-rows", type=int, default=1_000_000)
    parser.add_argument("--large-file-groups", type=int, default=1_000)
    parser.add_argument("--large-file-repetitions", type=int, default=1)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not BINARY.is_file():
        raise SystemExit("build ./milena first (make all)")
    if not 1 <= args.repetitions <= 20:
        raise SystemExit("--repetitions must be between 1 and 20")
    if not 0 <= args.large_rows <= MAX_ROWS:
        raise SystemExit(f"--large-rows must be between 0 and {MAX_ROWS}")
    if args.large_file_bytes and args.large_file_bytes < 10_000_000_000:
        raise SystemExit("--large-file-bytes must be at least 10,000,000,000")
    if not 1 <= args.large_file_rows <= MAX_ROWS:
        raise SystemExit(f"--large-file-rows must be between 1 and {MAX_ROWS}")
    if args.large_file_groups != 1_000 or args.large_file_groups > args.large_file_rows:
        raise SystemExit("--large-file-groups must be exactly 1000 and not exceed rows; fewer groups may not force reducer flushes")
    if not 1 <= args.large_file_repetitions <= 5:
        raise SystemExit("--large-file-repetitions must be between 1 and 5")
    cases = [("small", 100, 4), ("medium", 10_000, 32)]
    if args.large_rows:
        cases.append(("large-opt-in", args.large_rows,
                      min(args.large_rows, 1_000)))
    payload = {
        "schema": "milena-grouped-spill-benchmark-v3",
        "workloads": [{"fixture": name, **run_case(rows, groups,
                                                       args.repetitions)}
                      for name, rows, groups in cases],
        "large_file_gate": (run_large_file_case(
            args.large_file_bytes, args.large_file_rows,
            args.large_file_groups, args.large_file_repetitions)
            if args.large_file_bytes else None),
        "environment": {"platform": platform.platform(),
                        "machine": platform.machine(),
                        "compiler": os.environ.get("CC", "make default CC"),
                        "commit": os.environ.get("GITHUB_SHA", "unknown")},
        "methodology": ("Deterministic generated CSV; canonical lexer-parser-AST-"
                        "semantic-runtime-stream backend; compare in-memory and "
                        "spill output values, per-process peak RSS (Windows peak working "
                        "set or resource.getrusage where available), scratch sampled every "
                        "10 ms, actual reducer spill byte/record and initial-run counters, "
                        "and wall-clock observations."),
        "limitations": [
            "Peak RSS is per-process child working set on Windows or ru_maxrss on Linux/macOS (null elsewhere), not aggregate cgroup/system RSS or a memory guarantee.",
            "Scratch peak is a 10 ms sampled sum of logical file sizes for files prefixed by the configured path; short-lived peaks can be missed and final scratch is cleaned up. Scratch bytes are on disk, not RAM.",
            "No SLO or performance guarantee; results apply only to the recorded environment and workload.",
            "The optional >=10 GB gate is a generated CSV with a repeated padding field, one million rows and exactly 1,000 groups; it checks full input consumption, output parity and sampled nonzero spill, but not arbitrary high-cardinality or distributed scale.",
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
