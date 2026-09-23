# Reproducible local scalability measurements

`benchmarks/grouped_spill_benchmark.py` generates deterministic CSV fixtures
and compares the canonical in-memory and spill routes at 100 rows and 10,000
rows by default. `--large-rows N` adds an opt-in scale up to 1,000,000 rows;
`--repetitions` controls repeated measurements (default 3). It validates the
row contract and group values before reporting elapsed wall time, backend time,
per-process peak RSS where `resource.getrusage(RUSAGE_CHILDREN)` is supported,
and a 10 ms sampled peak sum of scratch files whose names share the configured
spill-path prefix. Scratch is removed on success, so only in-run sampling can
observe it.

`--large-file-bytes 10000000000` enables the acceptance gate used by CI: it
writes a non-sparse generated CSV of at least 10,000,000,000 bytes, with one
million rows, exactly 1,000 groups and a repeated padding field. The 1,000-key
workload exceeds the configured reducer map capacity (262,144-byte budget), so
the spill route must write scratch runs to disk; the gate asserts nonzero sampled
scratch bytes and rejects any route/mode mismatch rather than silently falling
back to in-memory grouping. Both reports must match the generated file's exact
size in `bytes_entrada` and `bytes_leidos`, in addition to matching row/group/
value results. The gate reports logical file bytes and allocated disk bytes
separately; Linux/macOS use `st_blocks`, and Windows uses
`GetCompressedFileSizeW`. The large-file gate runs once by default because it
reads a 10 GB-class input twice; smaller workloads retain repeated measurements.
The file is temporary and is not committed or uploaded. Linux and Windows CI
both run this gate. The C stream, report-staging, spill recovery, and run-size
paths use 64-bit file positioning, including Windows `_ftelli64`/`_fseeki64`;
POSIX builds request 64-bit `off_t`.

This validates byte-volume streaming and a real local spill for this deterministic
1,000-key fixture, not arbitrary high-cardinality processing, arbitrary schemas,
a global RAM cap, or industrial/distributed Big Data capability.

The RSS observation is the child process's peak working set on Windows or
`ru_maxrss` (normalized to bytes on Linux/macOS), not aggregate RSS for a process
tree, cgroup, or system. It is not a global process/reducer memory guarantee.
The scratch measure is sampled and can miss short-lived peaks. Unsupported
platforms report RSS as `null`. The benchmark records the platform, architecture,
commit (when supplied by CI), input size, limits, repetitions, and exact per-run
observations in JSON.

These are reproducible measurements, not an SLO or a performance guarantee.
They apply only to the recorded environment, compiler, storage, and fixture.
The benchmark does not measure CSV-aware partition execution, Arrow/Parquet,
cloud or distributed execution.
