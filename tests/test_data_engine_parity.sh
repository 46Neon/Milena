#!/bin/sh
set -eu
MILENA_BIN="$(pwd)/milena"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/milena-data-parity.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM
cat > "$TMP_DIR/rows.csv" <<'CSV'
grupo,valor
B,2
A,1
A,3
C,4
A,not-a-number
B,
CSV
cat > "$TMP_DIR/in-memory.milena" <<'MILENA'
.analisis memoria {
  dataset cargar datos("rows.csv")
  variable grupo texto
  variable valor numerica
  .filtrar { #condicion("valor > 1") }
  .agrupar dataset { #por("grupo") #suma("valor") #media("valor") #conteo("valor") }
  .exportar { ("memory.json") }
}
MILENA
cat > "$TMP_DIR/csv-stream.milena" <<'MILENA'
.analisis flujo {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv" con grupos de 10 con filas hasta 100 con tiempo hasta 30000 ms
  filtrar "valor" > 1;
  agrupar por "grupo" resumir { suma de "valor"; media de "valor"; contar de "valor"; }
  guardar resultado en "stream.json"
}
MILENA
cat > "$TMP_DIR/in-memory-count.milena" <<'MILENA'
.analisis memoria_conteo {
  dataset cargar datos("rows.csv")
  variable grupo texto
  variable valor numerica
  .agrupar dataset { #por("grupo") #conteo("valor") }
  .exportar { ("memory-count.json") }
}
MILENA
cat > "$TMP_DIR/csv-count.milena" <<'MILENA'
.analisis flujo_conteo {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv" con grupos de 10 con filas hasta 100 con tiempo hasta 30000 ms
  agrupar por "grupo" resumir { contar de "valor"; }
  guardar resultado en "stream-count.json"
}
MILENA
(cd "$TMP_DIR" && "$MILENA_BIN" run in-memory.milena)
(cd "$TMP_DIR" && "$MILENA_BIN" run csv-stream.milena)
(cd "$TMP_DIR" && "$MILENA_BIN" run in-memory-count.milena)
(cd "$TMP_DIR" && "$MILENA_BIN" run csv-count.milena)
python3 - "$TMP_DIR" <<'PY'
import json, math, pathlib, sys
p = pathlib.Path(sys.argv[1])
memory = json.loads((p / "memory.json").read_text())
stream = json.loads((p / "stream.json").read_text())
mem_rows = {
    row["grupo"]: {
        "suma": row["valor_suma"],
        "media": row["valor_media"],
        "conteo": row["valor_conteo"],
    }
    for row in memory["datos"]
}
stream_rows = {
    row["clave"]: {
        metric["operacion"]: metric["valor"]
        for metric in row["metricas"]
    }
    for row in stream["resultados"]
}
expected = {
    "A": {"suma": 3, "media": 3, "conteo": 1},
    "B": {"suma": 2, "media": 2, "conteo": 1},
    "C": {"suma": 4, "media": 4, "conteo": 1},
}
assert mem_rows.keys() == stream_rows.keys() == expected.keys(), (mem_rows, stream_rows)
for key, values in expected.items():
    for operation, expected_value in values.items():
        a, b = mem_rows[key][operation], stream_rows[key][operation]
        assert math.isclose(float(a), expected_value, rel_tol=0, abs_tol=1e-12), (key, operation, a)
        assert math.isclose(float(b), expected_value, rel_tol=0, abs_tol=1e-12), (key, operation, b)

memory_count = json.loads((p / "memory-count.json").read_text())
stream_count = json.loads((p / "stream-count.json").read_text())
mem_counts = {row["grupo"]: row["valor_conteo"] for row in memory_count["datos"]}
stream_counts = {
    row["clave"]: row["metricas"][0]["valor"]
    for row in stream_count["resultados"]
}
expected_counts = {"A": 2, "B": 1, "C": 1}
assert mem_counts == stream_counts == expected_counts, (mem_counts, stream_counts)
PY

# Materialized typed plans with invalid schema/operator/output declarations
# must fail during HIR preflight, before the intentionally missing CSV is
# touched. This keeps the failure deterministic and distinguishes it from I/O.
assert_preflight_rejects() {
  script=$1
  log="$TMP_DIR/$script.log"
  if (cd "$TMP_DIR" && "$MILENA_BIN" run "$script") >"$log" 2>&1; then
    echo "el plan inválido $script produjo éxito" >&2
    exit 1
  fi
  if grep -Eiq 'No se pudo abrir.*dataset|unable to open.*dataset|cannot open.*dataset' "$log"; then
    echo "el plan $script intentó abrir el CSV antes de fallar en preflight" >&2
    cat "$log" >&2
    exit 1
  fi
}
cat > "$TMP_DIR/preflight-unknown-key.milena" <<'MILENA'
.analisis preflight_clave_desconocida {
  dataset cargar datos("missing-preflight.csv")
  variable grupo texto
  variable valor numerica
  .agrupar dataset { #por("columna_inexistente") #suma("valor") }
  .exportar { ("unknown-key.json") }
}
MILENA
cat > "$TMP_DIR/preflight-unknown-metric.milena" <<'MILENA'
.analisis preflight_metrica_desconocida {
  dataset cargar datos("missing-preflight.csv")
  variable grupo texto
  variable valor numerica
  .agrupar dataset { #por("grupo") #suma("columna_inexistente") }
  .exportar { ("unknown-metric.json") }
}
MILENA
cat > "$TMP_DIR/preflight-wrong-metric-type.milena" <<'MILENA'
.analisis preflight_tipo_metrica {
  dataset cargar datos("missing-preflight.csv")
  variable grupo texto
  variable valor texto
  .agrupar dataset { #por("grupo") #suma("valor") }
  .exportar { ("wrong-metric.json") }
}
MILENA
cat > "$TMP_DIR/preflight-unsupported-filter.milena" <<'MILENA'
.analisis preflight_operador_no_soportado {
  dataset cargar datos("missing-preflight.csv")
  variable grupo texto
  variable valor numerica
  .filtrar { #condicion("valor >= 1") }
  .agrupar dataset { #por("grupo") #suma("valor") }
  .exportar { ("unsupported-filter.json") }
}
MILENA
cat > "$TMP_DIR/preflight-invalid-output.milena" <<'MILENA'
.analisis preflight_salida_invalida {
  dataset cargar datos("missing-preflight.csv")
  variable grupo texto
  variable valor numerica
  .agrupar dataset { #por("grupo") #suma("valor") }
  .exportar { ("directory/") }
}
MILENA
cat > "$TMP_DIR/preflight-invalid-transform.milena" <<'MILENA'
.analisis preflight_transformacion_invalida {
  dataset cargar datos("missing-preflight.csv")
  variable grupo texto
  variable valor numerica
  variable factor texto
  .transformar dataset { #total("valor * factor") }
  .agrupar dataset { #por("grupo") #suma("valor") }
  .exportar { ("invalid-transform.json") }
}
MILENA
assert_preflight_rejects preflight-unknown-key.milena
assert_preflight_rejects preflight-unknown-metric.milena
assert_preflight_rejects preflight-wrong-metric-type.milena
assert_preflight_rejects preflight-unsupported-filter.milena
assert_preflight_rejects preflight-invalid-output.milena
assert_preflight_rejects preflight-invalid-transform.milena

# A source-row budget breach is a controlled failure. A previously published
# destination must survive unchanged rather than becoming a partial success.
printf '%s\n' 'previous-complete-report' > "$TMP_DIR/stable.json"
cat > "$TMP_DIR/limited.milena" <<'MILENA'
.analisis limite {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv" con filas hasta 2 con tiempo hasta 30000 ms
  agrupar por "grupo" resumir { suma de "valor"; }
  guardar resultado en "stable.json"
}
MILENA
if (cd "$TMP_DIR" && "$MILENA_BIN" run limited.milena); then
  echo 'el presupuesto de filas produjo éxito en vez de error' >&2
  exit 1
fi
[ "$(cat "$TMP_DIR/stable.json")" = 'previous-complete-report' ] || {
  echo 'un error de límite reemplazó el reporte completo anterior' >&2
  exit 1
}
