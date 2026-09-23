#!/bin/sh
set -eu
MILENA_BIN="$(pwd)/milena"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/milena-group-spill.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM
cat > "$TMP_DIR/rows.csv" <<'CSV'
grupo,valor
Z,1.5
A,2.25
A,
,3.75
Z,
B,
Bad,not-a-number
CSV
cat > "$TMP_DIR/one.milena" <<EOF
.analisis agrupacion {
  dataset cargar datos("rows.csv")
  variable grupo texto
  variable valor numerica
  .agrupar dataset { #por("grupo") #suma("valor") }
  .exportar { ("one.json") }
}
EOF
cat > "$TMP_DIR/spill.milena" <<EOF
.analisis agrupacion {
  dataset cargar datos("rows.csv")
  variable grupo texto
  variable valor numerica
  .agrupar dataset { #por("grupo") #spill("$TMP_DIR/scratch.bin", 262144, 1048576, 128, 32) #suma("valor") }
  .exportar { ("one.json") }
}
EOF
(cd "$TMP_DIR" && "$MILENA_BIN" run one.milena)
cp "$TMP_DIR/one.json" "$TMP_DIR/memory.json"
(cd "$TMP_DIR" && "$MILENA_BIN" run spill.milena)
python3 - "$TMP_DIR" <<'PY'
import json, pathlib, sys
p=pathlib.Path(sys.argv[1])
a=json.loads((p/"memory.json").read_text())
b=json.loads((p/"one.json").read_text())
def rows(doc): return {r["grupo"]: r["valor_suma"] for r in doc["datos"]}
expected={"Z":1.5,"A":2.25,None:3.75,"B":None,"Bad":None}
assert rows(a)==expected, rows(a)
assert rows(b)==expected, rows(b)
assert [r["grupo"] for r in b["datos"]]==[None,"A","B","Bad","Z"]
assert not (p/"scratch.bin").exists(), "spill no eliminado en éxito"
PY
# In-memory multi-metric behavior is retained; the same explicit spill request
# is rejected by semantic validation rather than silently falling back.
cat > "$TMP_DIR/multi.milena" <<EOF
.analisis agrupacion {
  dataset cargar datos("rows.csv")
  variable grupo texto
  variable valor numerica
  .agrupar dataset { #por("grupo") #suma("valor") #media("valor") }
  .exportar { ("multi.json") }
}
EOF
(cd "$TMP_DIR" && "$MILENA_BIN" run multi.milena)
python3 - "$TMP_DIR/multi.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1])); assert "valor_suma" in x["datos"][0] and "valor_media" in x["datos"][0]
PY
sed "s|#por(\"grupo\")|#por(\"grupo\") #spill(\"$TMP_DIR/multi.bin\", 262144, 1048576, 128, 32)|" "$TMP_DIR/multi.milena" > "$TMP_DIR/multi-spill.milena"
if (cd "$TMP_DIR" && "$MILENA_BIN" run multi-spill.milena); then
  echo 'spill aceptó más de una métrica' >&2; exit 1
fi
[ ! -e "$TMP_DIR/multi.bin" ]
# Disk quota and output-group quota failures must clean all scratch files.
sed "s|scratch.bin|quota.bin|; s|1048576|1|" "$TMP_DIR/spill.milena" > "$TMP_DIR/quota.milena"
if (cd "$TMP_DIR" && "$MILENA_BIN" run quota.milena); then
  echo 'la cuota spill insuficiente no falló' >&2; exit 1
fi
[ ! -e "$TMP_DIR/quota.bin" ]
sed "s|scratch.bin|groups.bin|; s|128, 32|128, 1|" "$TMP_DIR/spill.milena" > "$TMP_DIR/groups.milena"
if (cd "$TMP_DIR" && "$MILENA_BIN" run groups.milena); then
  echo 'la cuota de grupos no falló' >&2; exit 1
fi
[ ! -e "$TMP_DIR/groups.bin" ]
# Invalid typed resource arguments fail during parsing/semantic validation.
sed "s|scratch.bin|invalid.bin|; s|262144|0|" "$TMP_DIR/spill.milena" > "$TMP_DIR/invalid.milena"
if (cd "$TMP_DIR" && "$MILENA_BIN" run invalid.milena); then
  echo 'la política de recursos inválida fue aceptada' >&2; exit 1
fi
[ ! -e "$TMP_DIR/invalid.bin" ]
