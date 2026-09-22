# Streaming execution and local out-of-core grouping

The canonical lexer -> parser -> AST -> semantic -> runtime -> execution-plan -> stream backend supports bounded CSV summaries and typed grouped aggregation. Existing in-memory grouping remains the default.

When a typed plan configures `spill_directory`, `spill_partitions`, `spill_max_bytes`, and `spill_max_records`, grouped rows are written to local typed partition files with checksums, then read and merged deterministically. The implementation fails closed on I/O, truncation, checksum, cleanup, record, group, partition, and byte limits. Spilled output is deterministic by partition order and first-seen key order within each partition; it does not promise global first-seen order. This is local out-of-core processing only, not distributed processing and not Spark/Flink.
