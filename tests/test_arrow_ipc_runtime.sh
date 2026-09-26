#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$ROOT/tests/check_arrow_ipc_fixtures" "$ROOT/tests/fixtures/arrow_ipc"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/milena-arrow-ipc.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM
cp "$ROOT/tests/fixtures/arrow_ipc/generated_primitive_cpp21.stream" "$TMP_DIR/primitive.stream"
cp "$ROOT/tests/fixtures/arrow_ipc/int64_above_2_53_pyarrow23.stream" "$TMP_DIR/wide.stream"
cat > "$TMP_DIR/primitive.milena" <<'MILENA'
.analisis arrow_cpp_fixture {
  variable int64_nullable numerica
  variable float64_nullable numerica
  variable int64_nonnullable numerica
  datos desde "primitive.stream" formato arrow_stream
    procesar por lotes de 32 filas
    con lote hasta 33554432 bytes
    con columnas de 22
    con filas hasta 40
    con tiempo hasta 30000 ms
    con bytes hasta 67108864
    con salida hasta 67108864 bytes
  filtrar "int64_nonnullable" > 0;
  proyectar { "int64_nullable", "float64_nullable" }
  guardar resultado en "primitive-output.stream"
}
MILENA
(cd "$TMP_DIR" && "$ROOT/milena" run primitive.milena > primitive.stdout)
"$ROOT/tests/test_arrow_ipc" --verify-primitive "$TMP_DIR/primitive-output.stream"
grep -q 'Filas leídas: 37 | Filas escritas: 21' "$TMP_DIR/primitive.stdout"

cat > "$TMP_DIR/wide.milena" <<'MILENA'
.analisis int64_exactitud {
  variable wide numerica
  datos desde "wide.stream" formato arrow_stream
    procesar por lotes de 8 filas
    con lote hasta 1024 bytes
    con columnas de 1
    con filas hasta 5
    con tiempo hasta 30000 ms
    con bytes hasta 1024
    con salida hasta 1024 bytes
  filtrar "wide" > 9007199254740992;
  proyectar { "wide" }
  guardar resultado en "wide-output.stream"
}
MILENA
(cd "$TMP_DIR" && "$ROOT/milena" run wide.milena > wide.stdout)
"$ROOT/tests/test_arrow_ipc" --verify-wide "$TMP_DIR/wide-output.stream"
grep -q 'Filas leídas: 5 | Filas escritas: 2' "$TMP_DIR/wide.stdout"

# A failure after staging begins must leave the previous destination intact and
# remove the exclusive same-directory .part file.
cat > "$TMP_DIR/output-limit.milena" <<'MILENA'
.analisis salida_acotada {
  variable int64_nullable numerica
  datos desde "primitive.stream" formato arrow_stream
    procesar por lotes de 32 filas
    con lote hasta 33554432 bytes
    con columnas de 22
    con filas hasta 40
    con tiempo hasta 30000 ms
    con bytes hasta 67108864
    con salida hasta 1 bytes
  proyectar { "int64_nullable" }
  guardar resultado en "existing.stream"
}
MILENA
printf 'old-output-marker' > "$TMP_DIR/existing.stream"
if (cd "$TMP_DIR" && "$ROOT/milena" run output-limit.milena > output-limit.stdout 2>&1); then
  echo 'Arrow output byte limit unexpectedly succeeded' >&2
  exit 1
fi
[ "$(cat "$TMP_DIR/existing.stream")" = 'old-output-marker' ]
if find "$TMP_DIR" -maxdepth 1 -name 'existing.stream.part.*' -print -quit | grep -q .; then
  echo 'Arrow staging file leaked after output limit failure' >&2
  exit 1
fi
printf 'OK: Arrow IPC Milena CLI E2E\n'
