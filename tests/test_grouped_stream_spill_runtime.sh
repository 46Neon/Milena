#!/bin/sh
set -eu
MILENA_BIN="$(pwd)/milena"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/milena-stream-spill.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM
python3 - "$TMP_DIR" <<'PY'
import csv, pathlib, sys
p=pathlib.Path(sys.argv[1])
with (p/'rows.csv').open('w', newline='') as f:
    w=csv.writer(f)
    w.writerow(['grupo','valor'])
    w.writerow(['New, York\nMetro','1.5'])
    w.writerow(['New, York\nMetro','2.25'])
    w.writerow(['A',''])
    w.writerow(['A','not-a-number'])
    w.writerow(['B','3.75'])
    for i in range(40): w.writerow([f'key-{i:02d}', str(i + .125)])
PY
cat > "$TMP_DIR/memory.milena" <<'MILENA'
.analisis agrupacion_flujo {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv" con grupos de 100 con filas hasta 1000 con tiempo hasta 30000 ms
  agrupar por "grupo" resumir { suma de "valor"; }
  guardar resultado en "memory.json"
}
MILENA
cat > "$TMP_DIR/spill.milena" <<EOF_M
.analisis agrupacion_flujo {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv" con grupos de 100 con filas hasta 1000 con tiempo hasta 30000 ms
  agrupar por "grupo" #spill("$TMP_DIR/scratch.bin", 4096, 1048576, 128, 100, 1048576, 4096) resumir { suma de "valor"; }
  guardar resultado en "spill.json"
}
EOF_M
(cd "$TMP_DIR" && "$MILENA_BIN" run memory.milena)
(cd "$TMP_DIR" && "$MILENA_BIN" run spill.milena)
python3 - "$TMP_DIR" <<'PY'
import json, math, pathlib, sys
p=pathlib.Path(sys.argv[1])
a=json.loads((p/'memory.json').read_text())
b=json.loads((p/'spill.json').read_text())
def result(doc):
    return {r['clave']: r['metricas'][0] for r in doc['resultados']}
ma, sb=result(a), result(b)
assert set(ma)==set(sb), (len(ma),len(sb))
for k,memory_metric in ma.items():
    spill_metric=sb[k]
    v,w=memory_metric['valor'],spill_metric['valor']
    assert (v is None and w is None) or (v is not None and w is not None and math.isclose(v,w,rel_tol=1e-10,abs_tol=1e-10)), (k,v,w)
    assert memory_metric['valores_validos']==spill_metric['valores_validos']
    assert memory_metric['valores_nulos']==spill_metric['valores_nulos']
    assert memory_metric['valores_invalidos']==spill_metric['valores_invalidos']
assert sb['New, York\nMetro']['valor']==3.75
assert sb['A']['valor'] is None
assert sb['A']['valores_nulos']==1 and sb['A']['valores_invalidos']==1
assert [r['clave'] for r in b['resultados']]==sorted(sb, key=lambda x:x.encode())
assert b['grupos']==len(sb)
assert b['limite_salida_bytes']==1048576
assert not (p/'scratch.bin').exists()
PY
# COUNT remains an exact integer in the report and counts non-empty values,
# including text that would be invalid for a numeric metric.
cat > "$TMP_DIR/count.milena" <<EOF_M
.analisis conteo_exacto {
  variable grupo texto
  variable valor texto
  datos desde "rows.csv" con grupos de 100 con filas hasta 1000 con tiempo hasta 30000 ms
  agrupar por "grupo" #spill("$TMP_DIR/count.bin", 4096, 1048576, 128, 100, 1048576, 4096) resumir { contar de "valor"; }
  guardar resultado en "count.json"
}
EOF_M
(cd "$TMP_DIR" && "$MILENA_BIN" run count.milena)
python3 - "$TMP_DIR/count.json" <<'PY'
import json,sys
results={r['clave']:r['metricas'][0] for r in json.load(open(sys.argv[1]))['resultados']}
assert results['New, York\nMetro']['valor']==2
assert results['A']['valor']==1
assert results['A']['valores_nulos']==1
assert type(results['A']['valor']) is int
PY
[ ! -e "$TMP_DIR/count.bin" ]
# Successful publication atomically replaces an existing destination.
printf 'old-report' > "$TMP_DIR/spill.json"
(cd "$TMP_DIR" && "$MILENA_BIN" run spill.milena)
python3 - "$TMP_DIR/spill.json" <<'PY'
import json,sys
report=json.load(open(sys.argv[1]))
assert report['modo']=='flujo_agrupado_spill'
assert report['bytes_spill'] >= 0 and report['registros_spill'] >= 0
assert report['runs_spill'] >= 0
PY
[ ! -e "$TMP_DIR/scratch.bin" ]
! find "$TMP_DIR" -maxdepth 1 -name 'spill.json.part.*' | grep -q .
# Row quota, disk quota and group quota failures publish no report and clean spill state.
cat > "$TMP_DIR/rows-limit.milena" <<EOF_M
.analisis filas_limitadas {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv" con filas hasta 2 con tiempo hasta 30000 ms
  agrupar por "grupo" #spill("$TMP_DIR/rows-limit.bin", 4096, 1048576, 128, 100) resumir { suma de "valor"; }
  guardar resultado en "rows-limit.json"
}
EOF_M
if (cd "$TMP_DIR" && "$MILENA_BIN" run rows-limit.milena); then exit 1; fi
[ ! -e "$TMP_DIR/rows-limit.json" ] && [ ! -e "$TMP_DIR/rows-limit.bin" ]
cat > "$TMP_DIR/disk-limit.milena" <<EOF_M
.analisis cuota_limitada {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv" con filas hasta 1000 con tiempo hasta 30000 ms
  agrupar por "grupo" #spill("$TMP_DIR/disk-limit.bin", 4096, 200, 128, 100) resumir { suma de "valor"; }
  guardar resultado en "disk-limit.json"
}
EOF_M
if (cd "$TMP_DIR" && "$MILENA_BIN" run disk-limit.milena); then exit 1; fi
[ ! -e "$TMP_DIR/disk-limit.json" ] && [ ! -e "$TMP_DIR/disk-limit.bin" ]
! find "$TMP_DIR" -maxdepth 1 -name 'disk-limit.json.part.*' | grep -q .
cat > "$TMP_DIR/group-limit.milena" <<EOF_M
.analisis grupos_limitados {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv" con filas hasta 1000 con tiempo hasta 30000 ms
  agrupar por "grupo" #spill("$TMP_DIR/group-limit.bin", 4096, 1048576, 128, 2) resumir { suma de "valor"; }
  guardar resultado en "group-limit.json"
}
EOF_M
if (cd "$TMP_DIR" && "$MILENA_BIN" run group-limit.milena); then exit 1; fi
[ ! -e "$TMP_DIR/group-limit.json" ] && [ ! -e "$TMP_DIR/group-limit.bin" ]
! find "$TMP_DIR" -maxdepth 1 -name 'group-limit.json.part.*' | grep -q .
# The typed AST run-count cap rejects excess runs and removes all spill artifacts.
cat > "$TMP_DIR/run-limit.milena" <<EOF_M
.analisis runs_limitados {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv" con filas hasta 1000 con tiempo hasta 30000 ms
  agrupar por "grupo" #spill("$TMP_DIR/run-limit.bin", 4096, 1048576, 128, 100, 1048576, 1) resumir { suma de "valor"; }
  guardar resultado en "run-limit.json"
}
EOF_M
if (cd "$TMP_DIR" && "$MILENA_BIN" run run-limit.milena); then exit 1; fi
[ ! -e "$TMP_DIR/run-limit.json" ] && [ ! -e "$TMP_DIR/run-limit.bin" ]
! find "$TMP_DIR" -maxdepth 1 -name 'run-limit.json.part.*' | grep -q .
# Output report bytes are bounded before each complete JSON group is written;
# a failed replacement must leave the prior destination untouched.
printf 'old-report' > "$TMP_DIR/output-limit.json"
cat > "$TMP_DIR/output-limit.milena" <<EOF_M
.analisis salida_limitada {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv" con filas hasta 1000 con tiempo hasta 30000 ms
  agrupar por "grupo" #spill("$TMP_DIR/output-limit.bin", 4096, 1048576, 128, 100, 1024) resumir { suma de "valor"; }
  guardar resultado en "output-limit.json"
}
EOF_M
if (cd "$TMP_DIR" && "$MILENA_BIN" run output-limit.milena); then exit 1; fi
[ "$(cat "$TMP_DIR/output-limit.json")" = 'old-report' ]
[ ! -e "$TMP_DIR/output-limit.bin" ]
! find "$TMP_DIR" -maxdepth 1 -name 'output-limit.json.part.*' | grep -q .
# Time budget is checked during row ingestion and final callback.
python3 - "$TMP_DIR" <<'PY'
import csv,pathlib,sys
with (pathlib.Path(sys.argv[1])/'slow.csv').open('w',newline='') as f:
 w=csv.writer(f); w.writerow(['grupo','valor'])
 for i in range(50000): w.writerow(['same',i])
PY
cat > "$TMP_DIR/time-limit.milena" <<EOF_M
.analisis tiempo_limitado {
  variable grupo texto
  variable valor numerica
  datos desde "slow.csv" con filas hasta 100000 con tiempo hasta 1 ms
  agrupar por "grupo" #spill("$TMP_DIR/time-limit.bin", 4096, 1048576, 128, 10) resumir { suma de "valor"; }
  guardar resultado en "time-limit.json"
}
EOF_M
if (cd "$TMP_DIR" && "$MILENA_BIN" run time-limit.milena); then exit 1; fi
[ ! -e "$TMP_DIR/time-limit.json" ] && [ ! -e "$TMP_DIR/time-limit.bin" ]
! find "$TMP_DIR" -maxdepth 1 -name 'time-limit.json.part.*' | grep -q .
# Explicit typed rejection for unsupported key/metric types and multi-metric spill.
cat > "$TMP_DIR/invalid-type.milena" <<EOF_M
.analisis clave_invalida {
  variable grupo categorica
  variable valor numerica
  datos desde "rows.csv"
  agrupar por "grupo" #spill("$TMP_DIR/invalid-type.bin", 4096, 1048576, 128, 100) resumir { suma de "valor"; }
  guardar resultado en "invalid-type.json"
}
EOF_M
if (cd "$TMP_DIR" && "$MILENA_BIN" run invalid-type.milena); then exit 1; fi
[ ! -e "$TMP_DIR/invalid-type.json" ] && [ ! -e "$TMP_DIR/invalid-type.bin" ]
cat > "$TMP_DIR/invalid-metric.milena" <<EOF_M
.analisis metrica_invalida {
  variable grupo texto
  variable valor texto
  datos desde "rows.csv"
  agrupar por "grupo" #spill("$TMP_DIR/invalid-metric.bin", 4096, 1048576, 128, 100) resumir { suma de "valor"; }
  guardar resultado en "invalid-metric.json"
}
EOF_M
if (cd "$TMP_DIR" && "$MILENA_BIN" run invalid-metric.milena); then exit 1; fi
[ ! -e "$TMP_DIR/invalid-metric.json" ] && [ ! -e "$TMP_DIR/invalid-metric.bin" ]
cat > "$TMP_DIR/multi.milena" <<EOF_M
.analisis multi_metrica {
  variable grupo texto
  variable valor numerica
  datos desde "rows.csv"
  agrupar por "grupo" #spill("$TMP_DIR/multi.bin", 4096, 1048576, 128, 100) resumir { suma de "valor"; media de "valor"; }
  guardar resultado en "multi.json"
}
EOF_M
if (cd "$TMP_DIR" && "$MILENA_BIN" run multi.milena); then exit 1; fi
[ ! -e "$TMP_DIR/multi.json" ] && [ ! -e "$TMP_DIR/multi.bin" ]
# Canonical `milena run` filtering is evaluated by the CSV stream backend before
# both in-memory and spill grouping, without materializing selected rows.
python3 - "$TMP_DIR" <<'PY'
import csv,pathlib,sys
p=pathlib.Path(sys.argv[1])
with (p/'filter.csv').open('w',newline='') as f:
    w=csv.writer(f)
    w.writerow(['grupo','region','valor'])
    w.writerow(['New, York\nMetro','keep','1.5'])
    w.writerow(['New, York\nMetro','keep','2.25'])
    w.writerow(['A','skip','100'])
    w.writerow(['A','keep','1'])
PY
cat > "$TMP_DIR/filter-memory.milena" <<'MILENA'
.analisis filtro_en_memoria {
  variable grupo texto
  variable region texto
  variable valor numerica
  datos desde "filter.csv" con grupos de 100 con filas hasta 1000 con tiempo hasta 30000 ms
  filtrar "region" == "keep";
  agrupar por "grupo" resumir { suma de "valor"; }
  guardar resultado en "filter-memory.json"
}
MILENA
cat > "$TMP_DIR/filter-spill.milena" <<EOF_M
.analisis filtro_con_spill {
  variable grupo texto
  variable region texto
  variable valor numerica
  datos desde "filter.csv" con grupos de 100 con filas hasta 1000 con tiempo hasta 30000 ms
  filtrar "region" == "keep";
  agrupar por "grupo" #spill("$TMP_DIR/filter-scratch.bin", 4096, 1048576, 128, 100) resumir { suma de "valor"; }
  guardar resultado en "filter-spill.json"
}
EOF_M
(cd "$TMP_DIR" && "$MILENA_BIN" run filter-memory.milena)
(cd "$TMP_DIR" && "$MILENA_BIN" run filter-spill.milena)
python3 - "$TMP_DIR" <<'PY'
import json,pathlib,sys
p=pathlib.Path(sys.argv[1])
a=json.load(open(p/'filter-memory.json'))
b=json.load(open(p/'filter-spill.json'))
def values(doc):
    return {r['clave']:r['metricas'][0]['valor'] for r in doc['resultados']}
assert values(a)==values(b)=={'A':1,'New, York\nMetro':3.75}, (values(a),values(b))
assert b['runs_spill'] >= 0 and b['bytes_spill'] >= 0
PY
[ ! -e "$TMP_DIR/filter-scratch.bin" ]
# An empty match set still produces a valid empty grouped report through spill.
sed 's/== "keep"/== "missing"/' "$TMP_DIR/filter-spill.milena" > "$TMP_DIR/filter-empty.milena"
sed -i 's/filter-spill.json/filter-empty.json/' "$TMP_DIR/filter-empty.milena"
sed -i 's/filter-scratch.bin/filter-empty-scratch.bin/' "$TMP_DIR/filter-empty.milena"
(cd "$TMP_DIR" && "$MILENA_BIN" run filter-empty.milena)
python3 - "$TMP_DIR/filter-empty.json" <<'PY'
import json,sys
doc=json.load(open(sys.argv[1]))
assert doc['resultados']==[] and doc['grupos']==0
PY
[ ! -e "$TMP_DIR/filter-empty-scratch.bin" ]
# Typed numeric `>` predicates reject null/malformed fields by non-match and
# produce the same grouped result in the bounded-memory and spill backends.
cat > "$TMP_DIR/filter-numeric.csv" <<'CSV'
grupo,importe,valor
A,10,1
A,10.01,2
B,12,4
B,,100
B,no-numerico,100
A,nan,100
A,10,3
A,12x,100
CSV
cat > "$TMP_DIR/filter-numeric-memory.milena" <<'MILENA'
.analisis filtro_numerico_en_memoria {
  variable grupo texto
  variable importe numerica
  variable valor numerica
  datos desde "filter-numeric.csv" con grupos de 100 con filas hasta 1000 con tiempo hasta 30000 ms
  filtrar "importe" > 10;
  agrupar por "grupo" resumir { suma de "valor"; }
  guardar resultado en "filter-numeric-memory.json"
}
MILENA
cat > "$TMP_DIR/filter-numeric-spill.milena" <<EOF_M
.analisis filtro_numerico_con_spill {
  variable grupo texto
  variable importe numerica
  variable valor numerica
  datos desde "filter-numeric.csv" con grupos de 100 con filas hasta 1000 con tiempo hasta 30000 ms
  filtrar "importe" > 10;
  agrupar por "grupo" #spill("$TMP_DIR/filter-numeric-scratch.bin", 4096, 1048576, 128, 100) resumir { suma de "valor"; }
  guardar resultado en "filter-numeric-spill.json"
}
EOF_M
(cd "$TMP_DIR" && "$MILENA_BIN" run filter-numeric-memory.milena)
(cd "$TMP_DIR" && "$MILENA_BIN" run filter-numeric-spill.milena)
python3 - "$TMP_DIR" <<'PY'
import json,pathlib,sys
p=pathlib.Path(sys.argv[1])
a=json.load(open(p/'filter-numeric-memory.json'))
b=json.load(open(p/'filter-numeric-spill.json'))
def values(doc):
    return {r['clave']:r['metricas'][0]['valor'] for r in doc['resultados']}
assert values(a)==values(b)=={'A':2,'B':4}, (values(a),values(b))
PY
[ ! -e "$TMP_DIR/filter-numeric-scratch.bin" ]
