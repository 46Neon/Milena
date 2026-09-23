# Streaming execution and local grouped spill

The canonical lexer -> parser -> AST -> semantic -> runtime -> stream backend
supports bounded global CSV summaries and typed grouped aggregation. Language
programs can use grouped aggregation; the in-memory implementation is the
language default and enforces the configured group limit.

The C API `milena_stream_csv_grouped_with_options` additionally supports local
spill when its caller provides `spill_directory`, `spill_partitions`,
`spill_max_bytes`, and `spill_max_records`. Grouped input rows are written to
1–64 deterministic local partitions with typed values and FNV-1a checksums,
then read and merged; output keys are sorted with `strcmp`. The test suite
compares spilled results byte-for-byte with in-memory results except for the
`derramado` mode marker, checks spill counters and validates temporary-file
cleanup. Resource-limit and malformed/integrity errors fail closed.

Spill options are not currently exposed by the `.milena` syntax or the public
execution-plan options, so do not imply that a canonical language script
selects spill automatically. Final group accumulators remain in memory, bounded
by `max_groups * spill_partitions`; this is not constant-memory processing for
unbounded cardinality. This is local, single-process execution only, not
cluster/distributed execution, Spark, or Flink. Cloud backends, joins, columnar
formats, and ML are not implemented by this foundation.
