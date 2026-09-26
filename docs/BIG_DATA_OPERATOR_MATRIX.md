# Big-data operator contract (candidate Phase 2)

This page is the operator-by-operator contract for the **current candidate tree**. A `.milena` script reaches the canonical lexer, parser, typed AST, semantic validation and runtime, but the execution adapters are not yet one shared physical engine. Do not infer a format/backend capability from a matching operator name.

## Operator and storage matrix

| Operation | In-memory CSV / `MilenaTable` HIR | CSV streaming (`datos desde`) | Arrow IPC STREAM | SQLite |
|---|---|---|---|---|
| Numeric product (`#total`) | Implemented in the closed typed data-HIR subset; materializes the input table and result. | Rejected; no streaming projection/expression operator. | Rejected; no computed projection. | Not a table-data expression; raw SQL only or the separately documented typed slice. |
| Numeric filter | Typed HIR supports its closed predicate subset after CSV has been materialized. | One typed predicate at a time: text equality or numeric `>` in the grouped/global report plan. | One text-equality or numeric-`>` predicate in the Arrow candidate plan. | Typed SELECT/UPDATE have their own limited comparison rules; see SQLite docs. |
| Select/project | Typed HIR column selection, in-memory. | Rejected by the CSV stream plan. | Projection of declared numeric (`int64`/`float64`) and text columns, up to the Arrow profile limits. | Typed SELECT projection of declared columns, one table. |
| Clean nulls / duplicates | Typed HIR operations, in-memory and materialized. | Rejected. | Rejected. | Not implemented as typed operators. |
| Global aggregates | Typed HIR supports its documented aggregate subset over an in-memory table. | One-pass aggregate report; up to 64 metrics. | Rejected. | Raw SQL or the separately documented typed SELECT slice; not lowered from the common aggregate operator. |
| Grouped aggregates | Typed HIR groups a materialized table. | Grouped summary supports up to two TEXT keys only with explicit spill for the composite-key form; up to 64 metrics. Spill has explicit scratch quota, key-size, output-group, output-byte and run limits. | Rejected. | No typed GROUP BY operation in the candidate slice. |
| Join | One in-memory inner join, one key, one right CSV dataset; output is materialized. | Rejected; no external/streaming join. | Rejected. | No typed join in the SQLite slice. Raw SQL remains a separate explicit SQL surface. |
| Export | In-memory data-HIR emits its supported JSON table report. | CSV stream emits the documented JSON aggregate report. | Candidate plan writes Arrow IPC STREAM. | SQL results are materialized under SQL-plan row/byte/time limits. |

The parser/runtime reject unsupported combinations rather than silently replacing a requested stream operation with an in-memory table operation. Arrow IPC STREAM is work in progress and is **not** advertised as a verified/released capability; SQLite is a raw-SQL compatibility baseline plus a small typed slice, not a completed ORM. See [Arrow IPC STREAM profile](ARROW_IPC_STREAM_PROFILE.md) and [SQLite native backend](SQLITE_NATIVE_BACKEND.md) before relying on either.

## Resource limits and what they mean

- **In-memory table HIR:** the canonical runtime loads CSV into `Dataset`/`MilenaTable` before applying transforms. Binding records the observed input and operation-derived row/column policy; this is not an operator-configurable global memory, row, time, or scratch cap. Group cardinality is limited by available memory, not by a bounded-memory grouping guarantee. The in-memory join's `#limites(bytes, output_rows)` is a preflight for allocations controlled by that join, not a whole-process RSS bound; the right table is also loaded first. See [join resource limits](JOIN_RESOURCE_LIMITS.md).
- **CSV streaming:** rows are read as complete CSV records, not byte-partitioned. Source limits can cap record bytes, columns, rows and cooperative elapsed time; grouped processing can additionally cap groups. The spill policy caps reducer memory, scratch bytes, key bytes, output groups, report bytes and run count. These per-operation budgets are not a process-wide RSS guarantee. Output is staged and published only when successful; spill temporaries are removed on success and error. See [streaming execution](STREAMING_EXECUTION.md), [scalability benchmarks](SCALABILITY_BENCHMARKS.md), and [spill benchmark](../benchmarks/grouped_spill_benchmark.py).
- **Arrow IPC STREAM candidate:** bounded batches, input/output bytes, rows, columns and cooperative elapsed time are enforced according to the Arrow profile. Payload allocation limits do not include all schema/runtime/output allocations and therefore are not a process RSS ceiling. Output uses a same-directory staging file and replacement only after successful completion.
- **SQLite:** query/result row and logical-byte limits plus cooperative VM timeout are configurable within hard caps. They bound backend accounting, not process RSS, and a multi-operation SQL block does not make every successful earlier query output atomic with later operations. See [SQLite native backend](SQLITE_NATIVE_BACKEND.md).

A rejected operation returns an error rather than a success-shaped partial report. Atomicity applies at the documented operation/output boundary only; it does not imply a transaction across unrelated report files or an entire multi-operation script.

## End-to-end verification

`make test-data-engine-parity` executes two real `.milena` programs over the same synthetic CSV: the typed in-memory table path and the CSV streaming aggregate path. It compares `suma`, `media`, and `conteo` per key against identical expected values and checks that a row-budget failure preserves an existing output file. The grouped-spill runtime test separately compares the stream in-memory grouping with a forced spill request and checks scratch cleanup. `make test` includes these gates.

The opt-in million-row and 10-GB-class forced-spill measurements remain separate benchmark jobs; their elapsed time, peak RSS observation, file bytes, scratch observations and configured budgets are specific to those deterministic fixtures and CI hosts, not general guarantees. Existing measurement scripts/workflows must run on the exact candidate SHA before reporting benchmark results.

## Phase 2 boundary still open

This matrix deliberately exposes the current disjoint backend plans. The common typed operator contract is not yet lowered to one backend-neutral operator graph across `MilenaDataHIR`, CSV streaming, Arrow IPC and SQLite. In particular, streaming supports only the report/aggregation/filter subset above; no arbitrary operator chain, external join, multi-format automatic planner, or shared pushdown path is claimed. Phase 2 acceptance remains incomplete until the supported overlap is represented by a shared typed plan, broader parity and resource-failure tests run on the exact candidate revision, and the million-row and large-file gates are green.
