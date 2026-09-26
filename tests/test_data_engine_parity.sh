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
CSV
cat > "$TMP_DIR/in-memory.milena" <<'MILENA'
.analisis memoria {
  dataset cargar datos("rows.csv")
  variable grupo texto
  variable valor numerica
  .agrupar dataset { #por("grupo") #suma("valor") #media("valor") #conteo("valor") }
  .exportar { ("memory.json") }
}
MILENA
cat > "$TMP_DIR/csv-stream.milena" <<'MILENA'
.analisis flujo {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv" con grupos de 10 con filas hasta 100 con tiempo hasta 30000 ms
  agrupar por "grupo" resumir { suma de "valor"; media de "valor"; contar de "valor"; }
  guardar resultado en "stream.json"
}
MILENA
(cd "$TMP_DIR" && "$MILENA_BIN" run in-memory.milena)
(cd "$TMP_DIR" && "$MILENA_BIN" run csv-stream.milena)
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
    "A": {"suma": 4, "media": 2, "conteo": 2},
    "B": {"suma": 2, "media": 2, "conteo": 1},
    "C": {"suma": 4, "media": 4, "conteo": 1},
}
assert mem_rows.keys() == stream_rows.keys() == expected.keys(), (mem_rows, stream_rows)
for key, values in expected.items():
    for operation, expected_value in values.items():
        a, b = mem_rows[key][operation], stream_rows[key][operation]
        assert math.isclose(float(a), expected_value, rel_tol=0, abs_tol=1e-12), (key, operation, a)
        assert math.isclose(float(b), expected_value, rel_tol=0, abs_tol=1e-12), (key, operation, b)
PY
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
