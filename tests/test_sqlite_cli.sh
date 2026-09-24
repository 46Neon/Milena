#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
MILENA_BIN="${MILENA_BIN:-./milena}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
DB="$tmp/native.sqlite"
cat > "$tmp/sql.milena" <<MILENA
sql desde "$DB" {
  ejecutar "CREATE TABLE registros(id INTEGER PRIMARY KEY, texto TEXT, opcional INTEGER)";
  ejecutar "INSERT INTO registros VALUES(?,?,?)" con (9007199254740997, "inseguro'); DROP TABLE registros; --", nulo);
  ejecutar "INSERT INTO registros VALUES(?,?,?)" con (9007199254740998, "Milena — UTF-8", nulo);
  iniciar;
  ejecutar "INSERT INTO registros VALUES(?,?,?)" con (9007199254740999, "transacción confirmada", nulo);
  confirmar;
  iniciar;
  ejecutar "INSERT INTO registros VALUES(?,?,?)" con (88, "revertida explícitamente", nulo);
  revertir;
  consulta "SELECT id,texto,opcional FROM registros WHERE id=?" con (9007199254740997);
}
MILENA
"$MILENA_BIN" run "$tmp/sql.milena" > "$tmp/result.txt"
grep -q 'Tabla Milena: 1 filas, 3 columnas' "$tmp/result.txt"
grep -q '9007199254740997' "$tmp/result.txt"
grep -q 'inseguro' "$tmp/result.txt"
grep -q 'DROP TABLE registros' "$tmp/result.txt"
grep -q 'Milena' "$tmp/result.txt"

cat > "$tmp/typed-select.milena" <<MILENA
sql desde "$DB" {
  tabla registros (id entero, texto texto, opcional entero);
  seleccionar id, texto de registros donde id = 9007199254740997;
}
MILENA
"$MILENA_BIN" run "$tmp/typed-select.milena" > "$tmp/typed-select.txt"
grep -q 'Tabla Milena: 1 filas, 2 columnas' "$tmp/typed-select.txt"
grep -q '9007199254740997' "$tmp/typed-select.txt"
grep -q 'inseguro' "$tmp/typed-select.txt"
grep -q 'DROP TABLE registros' "$tmp/typed-select.txt"

cat > "$tmp/typed-insert.milena" <<MILENA
sql desde "$DB" {
  tabla registros (id entero, texto texto, opcional booleano);
  iniciar;
  insertar en registros (id, texto) valores (123, "typed data'); DROP TABLE registros; --");
  confirmar;
  seleccionar id, texto de registros donde id = 123;
}
MILENA
"$MILENA_BIN" run "$tmp/typed-insert.milena" > "$tmp/typed-insert.txt"
grep -q 'Tabla Milena: 1 filas, 2 columnas' "$tmp/typed-insert.txt"
grep -q '123' "$tmp/typed-insert.txt"
grep -q 'typed data' "$tmp/typed-insert.txt"
grep -q 'DROP TABLE registros' "$tmp/typed-insert.txt"

cat > "$tmp/typed-null-utf8.milena" <<MILENA
sql desde "$DB" {
  tabla registros (id entero, texto texto, opcional booleano);
  insertar en registros (id, texto, opcional) valores (125, "Milena — null tipado", nulo);
  actualizar registros establecer opcional = nulo donde id = 125;
  seleccionar id, texto, opcional de registros donde id = 125;
}
MILENA
"$MILENA_BIN" run "$tmp/typed-null-utf8.milena" > "$tmp/typed-null-utf8.txt"
grep -q '125' "$tmp/typed-null-utf8.txt"
grep -q 'Milena — null tipado' "$tmp/typed-null-utf8.txt"
grep -q 'nulo' "$tmp/typed-null-utf8.txt"

cat > "$tmp/typed-update.milena" <<MILENA
sql desde "$DB" {
  tabla registros (id entero, texto texto, opcional booleano);
  insertar en registros (id, texto) valores (124, "old value");
  iniciar;
  actualizar registros establecer texto = "committed value'); DROP TABLE registros; --" donde id = 123;
  confirmar;
  iniciar;
  actualizar registros establecer texto = "rolled back value" donde id = 123;
  revertir;
  seleccionar id, texto de registros donde id = 123;
  seleccionar id, texto de registros donde id = 124;
}
MILENA
"$MILENA_BIN" run "$tmp/typed-update.milena" > "$tmp/typed-update.txt"
grep -q 'Tabla Milena: 1 filas, 2 columnas' "$tmp/typed-update.txt"
grep -q 'committed value' "$tmp/typed-update.txt"
grep -q 'DROP TABLE registros' "$tmp/typed-update.txt"
grep -q 'old value' "$tmp/typed-update.txt"
if grep -q 'rolled back value' "$tmp/typed-update.txt"; then
  echo 'El UPDATE tipado explícitamente revertido dejó cambios visibles' >&2
  exit 1
fi

cat > "$tmp/typed-error-rollback.milena" <<MILENA
sql desde "$DB" {
  tabla registros (id entero, texto texto, opcional booleano);
  iniciar;
  insertar en registros (id, texto) valores (777, "rollback ante error tipado");
  insertar en registros (id, texto) valores (123, "clave primaria duplicada");
  confirmar;
}
MILENA
if "$MILENA_BIN" run "$tmp/typed-error-rollback.milena" > "$tmp/typed-error-rollback.out" 2>&1; then
  echo 'Se aceptó un INSERT tipado duplicado dentro de una transacción' >&2
  exit 1
fi
cat > "$tmp/verify-typed-error-rollback.milena" <<MILENA
sql desde "$DB" {
  consulta "SELECT count(*) AS total FROM registros WHERE id=777";
}
MILENA
"$MILENA_BIN" run "$tmp/verify-typed-error-rollback.milena" > "$tmp/verify-typed-error-rollback.out"
grep -Fxq '0' "$tmp/verify-typed-error-rollback.out"

LIMIT_DB="$tmp/typed-limit.sqlite"
cat > "$tmp/typed-limit-setup.milena" <<MILENA
sql desde "$LIMIT_DB" {
  ejecutar "CREATE TABLE limited(id INTEGER, texto TEXT)";
  ejecutar "INSERT INTO limited VALUES(1, 'repetido'), (2, 'repetido')";
}
MILENA
"$MILENA_BIN" run "$tmp/typed-limit-setup.milena" > "$tmp/typed-limit-setup.out"
cat > "$tmp/typed-row-limit.milena" <<MILENA
sql desde "$LIMIT_DB" limites filas 1 bytes 4096 tiempo 1000 {
  tabla limited (id entero, texto texto);
  seleccionar id, texto de limited donde texto = "repetido";
}
MILENA
if "$MILENA_BIN" run "$tmp/typed-row-limit.milena" > "$tmp/typed-row-limit.out" 2>&1; then
  echo 'Se ignoró el límite de filas en SELECT tipado' >&2
  exit 1
fi
grep -q 'Límite de filas SQLite excedido' "$tmp/typed-row-limit.out"
if grep -q 'Tabla Milena:' "$tmp/typed-row-limit.out"; then
  echo 'El SELECT tipado publicó una tabla parcial al exceder filas' >&2
  exit 1
fi

cat > "$tmp/typed-byte-limit.milena" <<MILENA
sql desde "$LIMIT_DB" limites filas 10 bytes 16 tiempo 1000 {
  tabla limited (id entero, texto texto);
  seleccionar id, texto de limited donde texto = "repetido";
}
MILENA
if "$MILENA_BIN" run "$tmp/typed-byte-limit.milena" > "$tmp/typed-byte-limit.out" 2>&1; then
  echo 'Se ignoró el límite de bytes en SELECT tipado' >&2
  exit 1
fi
grep -q 'Límite de bytes SQLite excedido' "$tmp/typed-byte-limit.out"
if grep -q 'Tabla Milena:' "$tmp/typed-byte-limit.out"; then
  echo 'El SELECT tipado publicó una tabla parcial al exceder bytes' >&2
  exit 1
fi

cat > "$tmp/open-transaction.milena" <<MILENA
sql desde "$DB" {
  iniciar;
  ejecutar "INSERT INTO registros VALUES(?,?,?)" con (77, "debe revertirse", nulo);
}
MILENA
if "$MILENA_BIN" run "$tmp/open-transaction.milena" > "$tmp/transaction.out" 2>&1; then
  echo 'Se aceptó una transacción incompleta' >&2
  exit 1
fi
grep -q 'La transacción SQL del plan está incompleta' "$tmp/transaction.out"
cat > "$tmp/verify.milena" <<MILENA
sql desde "$DB" {
  consulta "SELECT count(*) AS total FROM registros";
  consulta "SELECT name FROM sqlite_master WHERE type='table' AND name='registros'";
}
MILENA
"$MILENA_BIN" run "$tmp/verify.milena" > "$tmp/verify.out"
grep -q 'Tabla Milena: 1 filas, 1 columnas' "$tmp/verify.out"
grep -Fxq '6' "$tmp/verify.out"
grep -Fxq '"registros"' "$tmp/verify.out"
if grep -Eq 'debe revertirse|revertida explícitamente' "$tmp/verify.out"; then
  echo 'Una transacción no confirmada no se revirtió' >&2
  exit 1
fi

cat > "$tmp/limited.milena" <<MILENA
sql desde "$DB" limites filas 1 bytes 4096 tiempo 1000 {
  consulta "SELECT 1 UNION ALL SELECT 2";
}
MILENA
if "$MILENA_BIN" run "$tmp/limited.milena" > "$tmp/limited.out" 2>&1; then
  echo 'Se ignoró el límite SQL de filas configurado en el lenguaje' >&2
  exit 1
fi
grep -q 'Límite de filas SQLite excedido' "$tmp/limited.out"

cat > "$tmp/physical-setup.milena" <<MILENA
sql desde "$DB" {
  ejecutar "CREATE TABLE missing_column(id INTEGER)";
  ejecutar "CREATE TABLE extra_column(id INTEGER, nombre TEXT, sobra INTEGER)";
  ejecutar "CREATE TABLE wrong_type(id TEXT, nombre TEXT)";
  ejecutar "CREATE TABLE altered(id INTEGER, nombre TEXT)";
  ejecutar "CREATE TABLE bad_values(id INTEGER, reading REAL)";
  ejecutar "INSERT INTO bad_values VALUES(1, 1.5)";
  ejecutar "INSERT INTO bad_values VALUES(1, 'texto incompatible con REAL')";
  ejecutar "CREATE TABLE bad_boolean(id INTEGER, active INTEGER)";
  ejecutar "INSERT INTO bad_boolean VALUES(1, 1)";
  ejecutar "INSERT INTO bad_boolean VALUES(1, 2)";
}
MILENA
"$MILENA_BIN" run "$tmp/physical-setup.milena" > "$tmp/physical-setup.out"

expect_typed_schema_failure() {
  local name="$1"
  local table="$2"
  local declaration="$3"
  local columns="$4"
  local values="$5"
  cat > "$tmp/$name.milena" <<MILENA
sql desde "$DB" {
  tabla $table ($declaration);
  insertar en $table ($columns) valores ($values);
}
MILENA
  if "$MILENA_BIN" run "$tmp/$name.milena" > "$tmp/$name.out" 2>&1; then
    echo "La escritura tipada con esquema físico incompatible fue aceptada: $name" >&2
    exit 1
  fi
  grep -q 'esquema físico de SQLite no coincide\|tabla tipada debe existir' "$tmp/$name.out"
  if grep -q 'Tabla Milena:' "$tmp/$name.out"; then
    echo "Se publicó una tabla antes de rechazar el esquema físico: $name" >&2
    exit 1
  fi
}
expect_typed_schema_failure missing-table ausente 'id entero' 'id' '777'
expect_typed_schema_failure missing-column missing_column 'id entero, nombre texto' 'id, nombre' '777, "no debe escribirse"'
expect_typed_schema_failure extra-column extra_column 'id entero, nombre texto' 'id, nombre' '777, "no debe escribirse"'
expect_typed_schema_failure wrong-type wrong_type 'id entero, nombre texto' 'id, nombre' '777, "no debe escribirse"'

cat > "$tmp/schema-changed-before-typed-write.milena" <<MILENA
sql desde "$DB" {
  tabla altered (id entero, nombre texto);
  ejecutar "ALTER TABLE altered ADD COLUMN inesperada INTEGER";
  insertar en altered (id, nombre) valores (777, "no debe escribirse");
}
MILENA
if "$MILENA_BIN" run "$tmp/schema-changed-before-typed-write.milena" > "$tmp/schema-changed-before-typed-write.out" 2>&1; then
  echo 'Se aceptó el esquema físico modificado por SQL raw antes del INSERT tipado' >&2
  exit 1
fi
grep -q 'esquema físico de SQLite no coincide' "$tmp/schema-changed-before-typed-write.out"
if grep -q 'Tabla Milena:' "$tmp/schema-changed-before-typed-write.out"; then
  echo 'Se publicó una tabla ante el esquema alterado' >&2
  exit 1
fi

cat > "$tmp/verify-no-typed-writes.milena" <<MILENA
sql desde "$DB" {
  consulta "SELECT (SELECT count(*) FROM missing_column) + (SELECT count(*) FROM extra_column) + (SELECT count(*) FROM wrong_type) + (SELECT count(*) FROM altered) AS writes";
}
MILENA
"$MILENA_BIN" run "$tmp/verify-no-typed-writes.milena" > "$tmp/verify-no-typed-writes.out"
grep -Fxq '0' "$tmp/verify-no-typed-writes.out"

cat > "$tmp/typed-result-mismatch.milena" <<MILENA
sql desde "$DB" {
  tabla bad_values (id entero, reading real);
  seleccionar id, reading de bad_values donde id = 1;
}
MILENA
if "$MILENA_BIN" run "$tmp/typed-result-mismatch.milena" > "$tmp/typed-result-mismatch.out" 2>&1; then
  echo 'El SELECT tipado publicó valores incompatibles con su tipo declarado' >&2
  exit 1
fi
if grep -q 'Tabla Milena:' "$tmp/typed-result-mismatch.out"; then
  echo 'El SELECT tipado publicó una tabla parcial antes de fallar por tipo' >&2
  exit 1
fi

cat > "$tmp/typed-boolean-mismatch.milena" <<MILENA
sql desde "$DB" {
  tabla bad_boolean (id entero, active booleano);
  seleccionar active de bad_boolean donde id = 1;
}
MILENA
if "$MILENA_BIN" run "$tmp/typed-boolean-mismatch.milena" > "$tmp/typed-boolean-mismatch.out" 2>&1; then
  echo 'El SELECT tipado aceptó un booleano físico distinto de 0/1' >&2
  exit 1
fi
if grep -q 'Tabla Milena:' "$tmp/typed-boolean-mismatch.out"; then
  echo 'El SELECT tipado publicó una tabla booleana parcial antes de fallar' >&2
  exit 1
fi

echo 'SQLite canonical CLI end-to-end checks passed'
