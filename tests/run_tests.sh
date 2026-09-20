#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
project_dir=$(pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

cat > "$tmp_dir/ventas.csv" <<'CSV'
fecha,precio,cantidad
2026-01-01,10,2
2026-01-02,5,3
2026-01-03,2,4
CSV

./milena analizar "$tmp_dir/ventas.csv" "$tmp_dir/test-output.json" >/dev/null
test -s "$tmp_dir/test-output.json"
grep -q '"total": 43' "$tmp_dir/test-output.json"

cat > "$tmp_dir/arrays.milena" <<'MILENA'
arreglo valores_es = [1, 2, 3];
arreglo matriz_es = ceros(2, 3);
forma(valores_es);
forma(matriz_es);
array valores = [1, 2.5, 3];
array ceros = zeros(4);
shape(valores);
ndim(valores);
size(valores);
sum(valores);
mean(valores);
min(valores);
max(valores);
variance(valores);
std(valores);
median(valores);
percentile(valores, 90);
forma(valores);
dimensiones(valores);
tamaño(valores);
suma(valores);
media(valores);
minimo(valores);
maximo(valores);
varianza(valores);
desviacion_estandar(valores);
mediana(valores);
percentil(valores, 90);
array enteros = [1, 2, 3];
array otros = [4, 5, 6];
array uno = [10];
array matriz = zeros(2, 3);
array matriz_dos = zeros(2, 3);
array fila = zeros(1, 3);
array columna = zeros(2, 1);
array tablero = zeros(1, 3);
mean(matriz, 0);
mean(matriz, 0, true);
mean(matriz, 1);
min(matriz, 0);
max(matriz, 1);
variance(matriz, 0);
std(matriz, 1);
matriz + matriz_dos;
matriz + fila;
columna + tablero;
enteros + uno;
enteros + otros;
enteros - otros;
enteros * otros;
enteros / otros;
enteros + 2;
enteros - 2;
enteros * 2;
enteros / 2;
MILENA
./milena run "$tmp_dir/arrays.milena" > "$tmp_dir/arrays.out"
cat "$tmp_dir/arrays.out"
grep -q 'Array valores_es: dtype=int64, shape=(3), size=3' "$tmp_dir/arrays.out"
grep -q 'Array matriz_es: dtype=float64, shape=(2, 3), size=6' "$tmp_dir/arrays.out"
grep -q 'forma(valores_es) = (3)' "$tmp_dir/arrays.out"
grep -q 'forma(matriz_es) = (2, 3)' "$tmp_dir/arrays.out"
grep -q 'Array valores: dtype=float64, shape=(3), size=3' "$tmp_dir/arrays.out"
grep -q 'Array ceros: dtype=float64, shape=(4), size=4' "$tmp_dir/arrays.out"
grep -q 'Array uno: dtype=int64, shape=(1), size=1' "$tmp_dir/arrays.out"
grep -q 'Array matriz: dtype=float64, shape=(2, 3), size=6' "$tmp_dir/arrays.out"
grep -q 'Array matriz_dos: dtype=float64, shape=(2, 3), size=6' "$tmp_dir/arrays.out"
grep -q 'Operacion matriz + matriz_dos: dtype=float64, shape=(2, 3)' "$tmp_dir/arrays.out"
grep -q 'Operacion matriz + fila: dtype=float64, shape=(2, 3)' "$tmp_dir/arrays.out"
grep -q 'Operacion columna + tablero: dtype=float64, shape=(2, 3)' "$tmp_dir/arrays.out"
grep -q 'shape(valores) = (3)' "$tmp_dir/arrays.out"
grep -q 'ndim(valores) = 1' "$tmp_dir/arrays.out"
grep -q 'size(valores) = 3' "$tmp_dir/arrays.out"
grep -q 'sum(valores) = 6.5' "$tmp_dir/arrays.out"
grep -q 'mean(valores) = 2.1666666' "$tmp_dir/arrays.out"
grep -q 'min(valores) = 1' "$tmp_dir/arrays.out"
grep -q 'max(valores) = 3' "$tmp_dir/arrays.out"
grep -q 'variance(valores) = 0.72222222222222' "$tmp_dir/arrays.out"
grep -q 'std(valores) = 0.849836585' "$tmp_dir/arrays.out"
grep -q 'median(valores) = 2.5' "$tmp_dir/arrays.out"
grep -q 'percentile(valores, 90) = 2.899999' "$tmp_dir/arrays.out"
grep -q 'mean(matriz) = \[0, 0, 0\] shape=(3)' "$tmp_dir/arrays.out"
grep -q 'mean(matriz) = \[0, 0, 0\] shape=(1, 3)' "$tmp_dir/arrays.out"
grep -q 'mean(matriz) = \[0, 0\] shape=(2)' "$tmp_dir/arrays.out"
grep -q 'min(matriz) = \[0, 0, 0\] shape=(3)' "$tmp_dir/arrays.out"
grep -q 'max(matriz) = \[0, 0\] shape=(2)' "$tmp_dir/arrays.out"
grep -q 'variance(matriz) = \[0, 0, 0\] shape=(3)' "$tmp_dir/arrays.out"
grep -q 'std(matriz) = \[0, 0\] shape=(2)' "$tmp_dir/arrays.out"
grep -q 'Operacion enteros + uno: dtype=int64, shape=(3)' "$tmp_dir/arrays.out"
grep -q 'Operacion enteros + otros: dtype=int64, shape=(3)' "$tmp_dir/arrays.out"
grep -q 'Operacion enteros - otros: dtype=int64, shape=(3)' "$tmp_dir/arrays.out"
grep -q 'Operacion enteros \* otros: dtype=int64, shape=(3)' "$tmp_dir/arrays.out"
grep -q 'Operacion enteros / otros: dtype=int64, shape=(3)' "$tmp_dir/arrays.out"
grep -q 'Operacion enteros + 2: dtype=int64, shape=(3)' "$tmp_dir/arrays.out"
grep -q 'Operacion enteros - 2: dtype=int64, shape=(3)' "$tmp_dir/arrays.out"
grep -q 'Operacion enteros \* 2: dtype=int64, shape=(3)' "$tmp_dir/arrays.out"
grep -q 'Operacion enteros / 2: dtype=float64, shape=(3)' "$tmp_dir/arrays.out"

cat > "$tmp_dir/array-errors.milena" <<'MILENA'
array vacio = [];
MILENA
if ./milena run "$tmp_dir/array-errors.milena" > "$tmp_dir/array-errors.out" 2>&1; then
    echo 'Se aceptó un array vacío' >&2
    exit 1
fi
grep -q 'vacío' "$tmp_dir/array-errors.out"

cat > "$tmp_dir/unknown-array.milena" <<'MILENA'
array valores = [1];
shape(inexistente);
MILENA
if ./milena run "$tmp_dir/unknown-array.milena" > "$tmp_dir/unknown-array.out" 2>&1; then
    echo 'Se aceptó una variable de array inexistente' >&2
    exit 1
fi
grep -q 'Variable de array inexistente' "$tmp_dir/unknown-array.out"

cat > "$tmp_dir/invalid-operation.milena" <<'MILENA'
array valores = [1, 2, 3];
sum();
MILENA
if ./milena run "$tmp_dir/invalid-operation.milena" > "$tmp_dir/invalid-operation.out" 2>&1; then
    echo 'Se aceptó una operación sin argumento' >&2
    exit 1
fi
grep -q 'Argumento inválido' "$tmp_dir/invalid-operation.out"

cat > "$tmp_dir/array-overflow.milena" <<'MILENA'
array maximo = [9000000000000000000];
array uno = [9000000000000000000];
maximo + uno;
MILENA
if ./milena run "$tmp_dir/array-overflow.milena" > "$tmp_dir/array-overflow.out" 2>&1; then
    echo 'Se aceptó overflow int64' >&2
    exit 1
fi
grep -q 'fuera de rango' "$tmp_dir/array-overflow.out"

cat > "$tmp_dir/array-div-zero.milena" <<'MILENA'
array valores = [1];
array ceros = [0];
valores / ceros;
MILENA
if ./milena run "$tmp_dir/array-div-zero.milena" > "$tmp_dir/array-div-zero.out" 2>&1; then
    echo 'Se aceptó división por cero' >&2
    exit 1
fi
grep -q 'División por cero' "$tmp_dir/array-div-zero.out"

cat > "$tmp_dir/array-shape-error.milena" <<'MILENA'
array izquierdo = zeros(2, 2);
array derecho = zeros(3, 2);
izquierdo + derecho;
MILENA
if ./milena run "$tmp_dir/array-shape-error.milena" > "$tmp_dir/array-shape-error.out" 2>&1; then
    echo 'Se aceptaron shapes incompatibles' >&2
    exit 1
fi
grep -q 'formas no son compatibles' "$tmp_dir/array-shape-error.out"

cat > "$tmp_dir/clientes.csv" <<'CSV'
edad,ciudad,canal,visitas,compro
20,Caracas,web,3,1
22,Merida,web,4,1
25,Caracas,tienda,5,0
28,Valencia,web,6,1
31,Merida,tienda,7,0
35,Caracas,web,8,1
40,Valencia,tienda,9,0
45,Merida,web,10,1
CSV

sed \
  -e "s#datos/tu_archivo.csv#$tmp_dir/clientes.csv#g" \
  -e "s#reporte_clientes.json#$tmp_dir/reporte_clientes.json#g" \
  "$project_dir/examples/clasificacion_binaria.milena" \
  > "$tmp_dir/clasificacion_binaria.milena"

./milena run "$tmp_dir/clasificacion_binaria.milena" >/dev/null
test -s "$tmp_dir/reporte_clientes.json"
test -s "$tmp_dir/reporte_clientes.json.sst.json"
grep -q '"salidas_binarias"' "$tmp_dir/reporte_clientes.json"
grep -q '"unos": 5' "$tmp_dir/reporte_clientes.json"
grep -q '"histograma"' "$tmp_dir/reporte_clientes.json.sst.json"
grep -q '"pearson"' "$tmp_dir/reporte_clientes.json.sst.json"
grep -q '"normalidad"' "$tmp_dir/reporte_clientes.json.sst.json"

./milena perfil "$tmp_dir/clientes.csv" "$tmp_dir/perfil.json" >/dev/null
test -s "$tmp_dir/perfil.json"
printf 'OK: pruebas con datos sintéticos temporales completadas\n'
