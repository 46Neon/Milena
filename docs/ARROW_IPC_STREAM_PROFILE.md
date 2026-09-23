# Arrow IPC STREAM vertical — implementation status

**Status: work in progress; not yet a verified or released Milena capability.** The candidate tree contains an initial native C implementation wired into the canonical parser/AST/semantic/planner/runtime route. Zig 0.16.0's C frontend compiled the product and tests locally; the backend tests, CLI E2E fixtures, existing language runtime tests, sanitizers, and PyArrow 23.0.1 output interoperability checks passed in the authoring workspace. These are not native GCC/Clang CI results or validation of a published PR head. Native CI, actual target packaging, and Termux/AArch64 checks remain pending; do not advertise the capability as supported until the required exact-SHA and target gates pass.

## Deliberately narrow profile

The intended first vertical is a local Arrow IPC **STREAM** reader and STREAM writer, using Apache nanoarrow 0.9.0's official generated C bundle. The upstream source archive used to make that bundle is `apache-arrow-nanoarrow-0.9.0.tar.gz`, tagged `apache-arrow-nanoarrow-0.9.0`, SHA-256 `801200a0e95e869d5c4bdeb5b535dba58551482bb782b7dc8bd599c8b6e8cacf`. The bundle contains the core, IPC, and flatcc sources. The source tree's Apache-2.0 license and NOTICE and flatcc's third-party license are retained under `third_party/nanoarrow/`.

The current backend code is intentionally limited to:

- local regular files whose first IPC message is a stream schema, followed by zero or more record batches and the stream end marker;
- a bounded scalar struct schema with non-nested, non-dictionary, non-extension fields; unsupported scalar fields may remain unprojected;
- projection of only declared `numerica` (exact Arrow signed `int64` or `float64`) and `texto` (Arrow UTF-8 string) fields; there is no Milena Boolean declaration, so `binaria` is not mapped to Arrow bool;
- at most one typed predicate (`texto == "..."` or `numerica > literal`), whose declaration and Arrow schema type must agree;
- sequential batch processing and an IPC STREAM output.

Null predicate values do not match; projected nulls retain their validity. Root stream-schema metadata and metadata on projected fields are copied to the output schema; unprojected fields and their metadata are omitted. `int64` values are copied as signed 64-bit integers (not via `double`), preserving values above 2^53. The numeric filter literal is a Milena binary64 number; for `int64` fields the comparison is performed using `floor(literal)` to avoid converting each data value to binary64. String values are intended to be validated as UTF-8.

The current C backend rejects encoded input above 64 MiB, output above 64 MiB, more than 10,000,000 input rows, more than 65,536 rows in one source batch, more than 64 MiB of decoded Arrow input buffers in one batch (checked after nanoarrow returns the decoded batch), or more than 128 source columns. Language programs must specify explicit caps for input/output bytes, decoded batch bytes, rows, columns, batch rows, and elapsed time. The decoded-buffer byte check is not a resident-memory cap: it runs after decoding, excludes allocator capacity/slack, batch/view structs and output buffers, and cannot stop an allocation spike within a synchronous decode call. Nanoarrow allocations are not routed through a Milena memory-budget allocator, so peak resident memory is not independently bounded or measured and the full resource contract remains open. Processing is sequential; the time budget and cooperative cancellation callback are checked after each decoded batch and at row boundaries, so they prevent publication after an over-budget/cancelled decode but cannot preempt a synchronous nanoarrow decode call. No CLI/script cancellation signal is connected yet; therefore time/cancellation behavior still requires executable tests and review before support can be claimed.

## Intended syntax in the candidate

```milena
.analisis seleccionar_columnas {
  variable id numerica
  variable activo binaria
  datos desde "entrada.arrow" formato arrow_stream
    procesar por lotes de 4096 filas
    con lote hasta 33554432 bytes con columnas de 32 con filas hasta 100000
    con tiempo hasta 30000 ms con bytes hasta 67108864
    con salida hasta 67108864 bytes
  filtrar "id" > 9007199254740992;
  proyectar { "id", "activo"; }
  guardar resultado en "salida.arrow"
}
```

This is a development syntax proposal embodied in the candidate parser. `formato arrow_stream` means IPC streaming format only. The source and output are local paths; relative paths are resolved relative to the `.milena` script using the existing runtime path rules.

## Explicit exclusions

This profile does **not** mean support for Arrow IPC **file** format, Parquet, remote/object-store sources, projected bool or other unsupported scalar types, dictionaries, unsigned integers, decimals, timestamps, nested/list/struct fields, codecs/compression, SQL, general Arrow extension metadata, later PR phases, or full Arrow/industrial interoperability. CSV's reader and planner remain separate and unchanged.

## Atomic output and error contract in the implementation

The backend writes into an exclusive same-directory `.part.*` file, caps writes at the configured output-byte limit, writes the stream end marker, flushes and closes the staged file, then uses the platform's replace-rename operation. On an error or cancellation before publication it removes staging and does not publish a partial output. The backend test suite passed locally under Zig compilation and ASan/UBSan, including write/output limits, malformed/truncated input, cleanup, and existing-destination preservation. CLI E2E fixture tests also passed. Native compiler CI and target-specific rename semantics remain unverified.

## Missing verification before support can be claimed

1. Independent fixtures are checked in under `tests/fixtures/arrow_ipc/`, with source, producer versions, expected cases and SHA-256 hashes in that directory's README. The primitive and binary multi-batch inputs are Apache Arrow C++ 21.0.0 integration fixtures; a separate PyArrow/Arrow C++ 23.0.1 stream contains exact int64 values above 2^53. Fixture hashes and the CLI E2E tests passed locally; CI has not yet run on an updated PR head.
2. Backend tests cover multi-batch filtering/projection, null validity, exact int64, unsupported selected types/schema mismatch, malformed/truncated IPC, input/output/row/batch-row/batch-byte/column/time limits, cooperative cancellation, staging cleanup and old-output preservation. They passed locally with Zig's C frontend and under ASan/UBSan. The runtime script passed, and output was independently read with PyArrow 23.0.1 / Arrow C++ 23.0.1; these are local results, not native compiler CI.
3. `tests/test_arrow_ipc_runtime.sh` exercises the real `.milena` CLI over the independent multi-batch fixture and exact-int64 fixture; it is wired into `tests/run_tests.sh` and passed locally.
4. Full product and tests have not yet been validated with native GCC/Clang CI on the exact updated PR SHA. Physical Termux/AArch64, Windows, and target package/release validation remain pending.
5. The decoded-source-batch byte cap is checked only after nanoarrow synchronously decodes the batch; it is not a strict peak-resident-memory bound and cannot prevent a transient decoder allocation spike. Time and cooperative cancellation checks occur after decoding/at row boundaries; they cannot preempt synchronous decode, and no CLI/script signal is connected. These limitations are explicit unresolved resource-contract issues, not guarantees. Review and resolve or retain them as blockers before claiming support.
