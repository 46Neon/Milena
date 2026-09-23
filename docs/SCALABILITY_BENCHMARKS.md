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

The RSS observation is the child process's `ru_maxrss` (normalized to bytes on
Linux/macOS), not aggregate RSS for a process tree, cgroup, or system. It is not
a process-wide or reducer memory guarantee. The scratch measure is sampled and
can miss short-lived peaks. Unsupported platforms report RSS as `null`. The
benchmark records the platform, architecture, commit (when supplied by CI),
input size, limits, repetitions, and exact per-run observations in JSON.

These are reproducible measurements, not an SLO or a performance guarantee.
They apply only to the recorded environment, compiler, storage, and fixture.
The benchmark does not measure CSV-aware partition execution, Arrow/Parquet,
cloud or distributed execution.
