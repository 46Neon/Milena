# MLBC v1.3 data-plan ABI — wire-format foundation

> **Wire-format status:** the public v1.3 `DATA_PLAN_ONLY` encoder and standalone verifier are implemented. Focused SUM/COUNT format tests are integrated into `make test-bytecode` and bytecode CI; check the containing commit's CI result before calling those tests passing. Compiler lowering, data-run API/loader integration, CLI routing, and AOT remain **NOT IMPLEMENTED**. Scalar verification and VM execution remain v1.2-only; this wire-format increment does not enable data-HIR execution or claim VM/AOT parity. The existing data-HIR is still executed only by the canonical language runtime.

This proposal deliberately covers one vertical slice: one non-streaming CSV source, exactly one declared numeric column, exactly one global summary (`#suma` or `#conteo`) over that column, and one required JSON export. It is intended to make the next implementation small enough to verify and differentially test against the existing canonical data-HIR runtime.

## 1. Source-to-plan boundary and supported source

The source entry point parses `.milena` once through the existing canonical lexer, parser, semantic validation and `MilenaDataHIR` builder. A dedicated data-plan lowerer accepts only a complete HIR with all of these properties:

- one `dataset cargar datos("…csv")` source, dataset ID 1, not streaming;
- exactly one declaration, `variable <name> numerica`;
- exactly one `MILENA_HIR_DATA_SUMMARIZE` operation containing one aggregate, either `MILENA_AGG_SUM` or `MILENA_AGG_COUNT`, whose resolved HIR input name is that declared column;
- exactly one non-empty `.exportar` destination;
- no other declarations, operations, blocks, implicit defaults, or unrepresented statements.

Any deviation is an explicit unsupported/semantic error before a data module is emitted. In particular, `conteo` is restricted here to the declared numeric column even though the broader canonical HIR accepts count over other column types. The compiler derives the output column name exactly as the current HIR builder does: `<input>_suma` or `<input>_conteo`. It does not serialize AST pointers, C struct layouts, spans, source text, physical column indexes, or resolved array addresses. Column indexes are deliberately rebound against the loaded table at runtime.

Grammar-shaped example (the CSV has a `precio` header):

```milena
.analisis resumen {
  variable precio numerica
  dataset cargar datos("datos.csv")
  .resumir dataset { #suma("precio") }
  .exportar { ("resumen.json") }
}
```

`#conteo("precio")` is the only other summary form in this slice. The name and numeric declaration must agree exactly. The export is required rather than defaulted so the verifier sees the complete side-effect plan.

## 2. Versioned wire representation

Scalar MLBC v1.0/v1.1/v1.2 encodings remain byte-for-byte unchanged. The proposed data-only module is major 1, minor 3 and uses the existing 16-byte header, with a new, explicit v1.3 module-kind interpretation of fields that are still required to be zero by the legacy versions:

| Header bytes | v1.3 data-only meaning |
|---|---|
| `0..3` | ASCII `MLBC` |
| `4..5` | major `1` (u16 little-endian) |
| `6..7` | minor `3` (u16 little-endian) |
| `8..9` | register count `0` (u16) |
| `10..11` | flags `0x0001` = `DATA_PLAN_ONLY`; all other bits zero |
| `12..15` | instruction count `0` (u32) |

The payload starts at offset 16 and is one `DPLN` section. There are no scalar instructions, register tables, or hidden VM dispatch instructions in a data-only module. Length, version, kind and section size must all match exactly; truncation, trailing bytes, unknown flags, unknown section versions and mixed scalar/data modules are rejected. v1.0-v1.2 continue to require their existing header, instruction and type-trailer rules. A scalar `milena_bytecode_run` must reject this module kind; a future explicit data-run API handles it only after full verification.

All integers below are unsigned, fixed-width little-endian. Parsers must use checked addition/multiplication before deriving offsets or allocating. No host `size_t`, enum representation, struct padding, native-endian value, or NUL-terminated pointer is part of the wire ABI.

### 2.1 `DPLN` section

The section is exactly `section_bytes` long, including its 56-byte header, one 8-byte declared-column record, one 24-byte operation record, and the string table. The total module length is `16 + section_bytes`.

| Section offset | Width | Field and v1 contract |
|---:|---:|---|
| 0 | 4 | ASCII `DPLN` |
| 4 | 2 | section schema revision `1` |
| 6 | 2 | flags `0` |
| 8 | 4 | `section_bytes`, exact section length |
| 12 | 4 | source dataset ID `1` |
| 16 | 4 | declared-column count `1` |
| 20 | 4 | operation count `1` |
| 24 | 4 | max input file bytes, `1..67,108,864` (64 MiB) |
| 28 | 4 | max input data rows, `1..5,000` |
| 32 | 4 | max input columns, `1..70` |
| 36 | 4 | max CSV field bytes, `1..1,048,576` |
| 40 | 4 | max output rows, exactly `1` |
| 44 | 4 | string count, exactly `4` |
| 48 | 4 | string-table byte length, exact |
| 52 | 4 | reserved, exactly `0` |
| 56 | 8 | declared-column record |
| 64 | 24 | summary-operation record |
| 88 | variable | four-record string table; the section ends immediately after it |

The declared-column record is `u32 name_string_id` (must be 2), `u8 logical_type` (`1 = NUMBER`), `u8 flags` (zero), and `u16 reserved` (zero). It declares a numeric HIR input; CSV conversion and runtime binding must still confirm that the named header exists and is numeric. In particular, a BOOLEAN, text or categorical table column cannot be coerced to satisfy this declaration.

The operation record is `u8 operation` (`1 = SUM`, `2 = COUNT`), `u8 input_type` (`1 = NUMBER`), `u16 flags` (bit 0 is `SPAN_PRESENT`; all other bits zero), `u32 input_name_string_id` (must be 2), `u32 result_name_string_id` (must be 3), `u32 source_line`, `u32 source_column`, and `u32 reserved` (zero). If `SPAN_PRESENT` is set, line and column are one-based and nonzero; otherwise both are zero. The compiler records the aggregate's canonical HIR source location when available and rejects unrepresentable coordinates. Its result type is numeric. The verifier requires result name bytes to equal input name bytes followed by the operation's canonical suffix, `_suma` or `_conteo`; no alternate spelling or implicit output name is accepted.

### 2.2 Strings, exact bounds and verifier obligations

The string table contains exactly four records in fixed ID order: `0 = source path from the HIR`, `1 = export path from the HIR`, `2 = declared/input column name`, and `3 = HIR-derived output column name`. Each is encoded as `u32 byte_length` followed by exactly that many UTF-8 bytes (no terminator). The length prefixes plus string bytes equal `string_table_bytes`; all bytes are consumed, with no alignment or padding. Each string is at most 4,096 bytes, all four together at most 16,384 bytes, and embedded NUL or malformed UTF-8 is forbidden. Paths and names must be non-empty. The two column names must be valid canonical identifiers and satisfy the derived-name rule above. The whole data module is capped at 65,536 bytes.

The v1.3 verifier must check the header/module kind, exact sizes, fixed counts, reserved bits, every enum, every string boundary and encoding, all required string IDs, numeric schema/operation compatibility, canonical result name, and each encoded resource limit against its hard cap. It must reject unknown operation kinds rather than skip them. Verification is over the exact bytes later passed to execution; an invalid plan must cause no file open, output creation, or other side effect. Compiler-created modules also pass this same verifier before being returned. The current `MILENA_BYTECODE_MAX_MINOR == 1.2` and scalar-only verifier are unchanged today; this proposal does not imply v1.3 currently verifies.

## 3. Host loader, paths and provenance

A future explicit data-run API consumes verified bytes plus an execution context containing the `.milena` source filename for CLI-relative resolution and optional limits that can only lower the encoded caps. It copies or bounds-checks the four strings before use and never retains caller pointers after returning.

Path resolution must reuse the canonical data runtime's documented CLI behavior rather than reparsing source or inventing a second grammar:

- Absolute paths are used as supplied.
- For a relative **input** path, use an existing path relative to the process working directory if present; otherwise resolve it against the directory containing the `.milena` source filename. Failure to open the resolved input is an I/O error.
- A relative **export** path resolves against the `.milena` source directory. Absolute export paths remain absolute.
- Keep the original HIR path string distinct from the resolved filesystem path. The loader stamps `MILENA_HIR_DATASET_PATH_METADATA` (`milena.hir.dataset.path`) on the freshly loaded table with the original source-path string. Before binding/execution, the data adapter requires source dataset ID 1 and an exact match between this loader-stamped provenance and the plan's source-path string. Callers cannot supply or substitute an arbitrary table for the data VM path.

This compatibility resolver is not a filesystem sandbox: `..`, symlinks and absolute paths retain the host process's ordinary filesystem authority. Runtime documentation and CLI diagnostics must not claim otherwise. Before export, reject a destination that resolves to the same file as the input (lexical equality and, when the destination exists, same device/inode); do not truncate the source CSV. The loader's provenance establishes which declared path this invocation loaded, not a content hash or immutable snapshot; concurrent source replacement is not prevented by this ABI.

The host loader reads CSV with comma delimiter and the canonical `Dataset`/typed-table conversion path. It must enforce the encoded and hard limits while reading, not merely after unbounded allocation: a 64 MiB total input-file budget, 5,000 data rows, 70 columns, and 1 MiB per field. The additive `dataset_load_csv_with_limits_and_byte_budget` API is the bounded-loader foundation: it counts physical input bytes during reads (header, CSV syntax, and line terminators included), accepts exact-boundary EOF, and returns a structured `LIMIT` status for byte/row/column/field/record limits without replacing the destination dataset on failure. The API's byte ceiling is caller-supplied; the future v1.3 host adapter must select the encoded/hard maximum and may lower it, never raise it. This foundation is not a data-plan host adapter, VM execution path, or implementation of bytecode v1.3. No network/cloud source, stream mode, second source, or alternate reader is represented.

Build a `MilenaSchema` with the declared column as `MILENA_VAR_NUMERIC` and the existing numeric runtime role, then materialize with `milena_table_from_dataset`. This preserves canonical CSV conversion, null validity and FLOAT64 numeric storage. The runtime checks the loaded header and typed column against the declaration; undeclared CSV columns follow existing canonical conversion behavior and are not selectable by this plan. Plan maxima and host-supplied maxima are intersected (`min`); a caller may reduce, never raise, the plan or hard limit. Output is exactly one row and one numeric column. These limits bound input and representation sizes; they are not a promise of a process-wide RSS cap, and allocator/parser overhead must be described separately.

## 4. Shared canonical kernel and ownership

The VM data host adapter must not call `milena_run_dataset_program`, the legacy AST interpreter, or `milena_canonical_program_parse` during execution. Compilation uses canonical source/HIR once; the verified `DPLN` bytes are the sole execution plan. Data loading parses the referenced CSV, not the `.milena` source.

The operation must use the same existing typed table kernel used by `milena_canonical_program_execute_data` for summary HIR:

```c
MilenaAggregateSpec spec = {
    .value_column = input_column_name,
    .operation = operation == SUM ? MILENA_AGG_SUM : MILENA_AGG_COUNT,
    .output_name = result_column_name
};
status = milena_table_summarize(&summary, &input_table, &spec, 1, error);
```

Do not reimplement sum/count, CSV numeric parsing, null handling, overflow behavior, or JSON value formatting in a bytecode-specific loop. The canonical table kernel owns those semantics: `COUNT` counts valid/non-null input cells; `SUM` uses the kernel's checked/compensated implementation for the actual typed column and preserves its empty/all-null result and error behavior. Differential tests compare it directly with the canonical runtime rather than asserting a second arithmetic implementation.

For JSON, use the existing typed reporter `analysis_table_report` on the one-row summary table, with a schema that produces the same inferred numeric `variable` role as the current canonical runtime. Publish through a temporary file in the destination directory and atomic rename only after the complete report succeeds; an existing destination remains untouched on every earlier error. Do not use legacy `dataset_save_json`, which serializes the legacy string dataset format rather than the canonical typed report expected here.

All per-run `Dataset`, input/output `MilenaTable`, `MilenaSchema`, file handles, temporary path buffers and copied plan strings are owned by the data-run call and released on every return path. Input/output kernel tables are initialized and must not alias. Destroy partially initialized values safely; remove an unpublished temporary report on errors. Initialize the result/diagnostic before work, preserve the originating `MilenaStatus`/`MilenaError` stage and source-span information available at compile time, and never report success or publish a partial report after cleanup fails.

## 5. Errors and limits

There are three fail-closed phases:

1. **Compile/lower:** canonical parse/semantic/type failures retain their source diagnostics. Any valid canonical data-HIR outside this exact shape returns unsupported; it is not sent to `run` or the scalar VM.
2. **Verify:** bad magic/version/module kind/section, nonzero reserved field, trailing/truncated bytes, bad UTF-8/reference/name/type/opcode, or raised/zero/impossible cap returns a bytecode format/type/limit error before I/O.
3. **Execute:** missing/unreadable CSV and inaccessible output are I/O errors; missing/duplicate headers, nonnumeric binding and malformed CSV retain canonical data/type errors; byte/row/column/field overflows are limit errors; table arithmetic overflow, allocation failure and report/rename failure retain their canonical status. No result table or output JSON is exposed on error, and the previous output file is preserved.

Do not squeeze data failures into the scalar VM's `double` result or claim that scalar `milena_bytecode_run` executes a data module. A future `milena_bytecode_run_data`-style API should return an explicit status plus `MilenaError`/stage diagnostics and should only be added with its public ownership contract.

## 6. AOT boundary

No data AOT support is implemented or implied. The existing Linux x86-64 native backend is a scalar code generator; it must reject a v1.3 data-only module until a separately tested data-AOT design exists. When added, native AOT must use the same v1.3 verifier, immutable serialized plan and shared host loader/table/report kernel path as VM; it must not emit a private sum/count/CSV/JSON implementation or silently fall back to the interpreter. Its executable must define the input/output path context explicitly and be source-independent after build. Linux x86-64 linking must include the shared data host adapter plus the canonical dataset/CSV reader, schema conversion, `MilenaTable`/array kernel, JSON reporter and their common dependencies; Windows and Termux builds must continue to exclude/reject unsupported native AOT targets. VM/AOT data parity is a separate acceptance gate, not a consequence of this ABI or of scalar AOT parity.

## 7. Acceptance tests required before claiming implementation

Use a fixed small fixture such as `valor\n1.5\n2.5\n\n` (or an equivalent fixture with a documented null row) and a source program with one declared numeric `valor`, one `#suma("valor")`, and JSON export. Run both the existing canonical `milena run` path and `milena vm` from the same source context with separate output names, then compare the typed JSON outputs byte-for-byte and assert one row, one numeric `valor_suma` field and value `4`. Add an equivalent `#conteo("valor")` case and assert the existing kernel's valid-cell count. The result must originate from canonical `MilenaDataHIR` lowering and `milena_table_summarize`; no source reparse is permitted after module creation.

Required negative/limit/cleanup coverage includes: unsupported stream input; missing or second source; no, multiple, wrong-type or unrelated declarations; no export; zero/two summary aggregates; product/filter/group/join/SST or any extra operation; mismatched/absent CSV header; malformed/duplicate CSV header; malformed/trailing/truncated module; unknown/reserved flags/opcodes; bad UTF-8, invalid string references and lengths; noncanonical output name; each configured maximum and maximum-plus-one for bytecode/string/file/record/rows/columns; unreadable input; output aliasing input; output-open, report-write and rename failure; kernel overflow/allocation failure where injectable; and success after each injected failure. Assert that verification failures do not touch either path, failed executions leave an existing export byte-for-byte unchanged, temporary files/tables/handles are cleaned up, and sanitizer runs show no leaks/use-after-free. Add the tests to the focused bytecode and CLI targets. Only after these tests pass may the CLI be described as supporting this one data-HIR VM slice; AOT needs the separate parity/link acceptance gate above.
