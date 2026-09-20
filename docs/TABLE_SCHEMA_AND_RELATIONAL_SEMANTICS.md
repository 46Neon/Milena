# MilenaTable: schema and relational semantics

## Representation and ownership

`MilenaTableColumn` keeps its legacy prefix (`name`, `values`, `validity`) and adds an explicit `MilenaColumnType`:

- `MILENA_COLUMN_ARRAY`: any existing `MilenaArray` dtype. Relational ordering and arithmetic aggregation support the real dtypes (`bool`, signed/unsigned integers, `float32`, `float64`); complex columns can still be stored and copied.
- `MILENA_COLUMN_STRING`: owned, individually allocated, NUL-terminated UTF-8 strings. The table never borrows caller strings.
- `MILENA_COLUMN_CATEGORICAL`: an owned unsigned-code `MilenaArray` plus an owned, unique UTF-8 dictionary. Codes are checked for dictionary range on valid rows.

Column names are nonempty UTF-8 and unique. All columns have exactly `row_count` rows. A validity bitmap is owned by each nonempty column. `nullable` is schema information and is necessarily true when a null is present. Table and column metadata are owned UTF-8 key/value copies.

`milena_table_validate` is the central invariant checker. `init`/`destroy` tolerate repeated calls; `clone` performs a deep clone (arrays, strings, dictionaries, validity and metadata). There is no shallow table copy API. A table and all returned pointers remain owned by the table until mutation/destruction.

## Output and error contract

Every operation producing a table builds a private temporary and commits only after success. A previously valid output remains unchanged on failure. Output descriptor aliasing with an input is rejected. Initialized outputs may be reused; legacy uninitialized output descriptors remain accepted on successful calls for source compatibility, although explicit `{0}` plus `milena_table_init` is recommended.

Size/capacity arithmetic is checked. Arrays are accessed through public `MilenaArray` APIs and their public 1-D stride; no storage internals are read. Thus sliced/reversed/non-contiguous input columns and masks are handled correctly. Materialized table columns are writable contiguous copies.

## Null and NaN

Nullness is exclusively the validity bitmap. NaN is a valid floating value and is never silently treated as null.

- filter preserves validity;
- fill-null changes only null rows and supports all real array dtypes, strings, and categorical codes;
- drop-null can inspect all columns or an explicit subset;
- sort places null after non-null in both ascending and descending order;
- sort places NaN after finite values but before null in both directions; all NaNs tie, preserving input order;
- group-by makes all null values in a key position one group, and makes all NaN payloads one key;
- joins use SQL-like null semantics: a key containing null never matches another key (including null).

## Ordering

`milena_table_sort_keys` uses a stable bottom-up mergesort: `O(n log n)` comparisons and `O(n)` row-index storage. Multiple keys are applied lexicographically. Descending reverses finite/string/category value comparison, not the null or NaN placement. Strings and category labels use bytewise UTF-8 (`strcmp`) ordering. Equal keys preserve original order.

## Group-by and aggregates

`milena_table_group_by` accepts multiple keys and multiple aggregate specs. An open-addressed deterministic hash index assigns groups in first-row appearance order. Expected complexity is `O(n * key_count + n * aggregate_count)` and storage is `O(n + groups)`.

Supported operations:

- `COUNT`: count valid values, output `int64`, checked against `INT64_MAX`;
- `SUM`: checked signed/unsigned accumulation without converting integers through floating point; bool sum outputs `uint64`; floating sums use compensated summation;
- `MEAN`: `float64`, compensated accumulation (the conversion to floating output is explicit);
- `MIN` / `MAX`: original real dtype.

An all-null group has COUNT zero and null SUM/MEAN/MIN/MAX. Floating NaN participates as a valid value: SUM/MEAN propagate it and MIN/MAX propagate a seen NaN. Integer overflow returns `MILENA_ERR_OVERFLOW` without changing the output.

The legacy single-key/single-aggregate wrapper is retained.

## Joins

`milena_table_join` implements inner, left, right and full equi-joins with one or more keys using a deterministic hash index. Key logical types must agree; array key dtypes must match exactly. Categorical values compare by label, allowing independently owned equivalent dictionaries. Duplicate matches produce the Cartesian product in left-row order, then right-row order. Right/full joins append unmatched right rows in original right order. Complexity is expected `O(left + right + output)` plus key comparison.

All left columns are emitted first. Every right column is also emitted. A colliding right name becomes `<name>_right`, then `<name>_right2`, etc. Outer-side missing values are null.

## Reshape

`milena_table_unpivot` is implemented. It emits rows in source-row order and value-column order, repeats identifier columns, creates a UTF-8 variable column, and retains value validity. Value columns must have the same logical/storage type; categorical dictionaries must be identical.

Pivot is intentionally not declared in this worker: a general collision/aggregation policy is not yet implemented.

## Limitations

- Relational ordering and arithmetic aggregation reject complex dtypes.
- String collation is deterministic bytewise UTF-8, not locale collation.
- Join numeric keys require identical dtypes; no implicit coercion occurs.
- Metadata is simple key/value data (no nested metadata or metadata deletion API).
- Pivot remains pending; unpivot is complete.
