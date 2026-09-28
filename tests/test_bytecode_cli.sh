#!/bin/sh
set -eu

cli=${1:-./milena}
no_native_cli=${2:-}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/milena-bytecode-cli.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
source_file="$tmpdir/arithmetic.milena"
executable="$tmpdir/native program"

expect_failure() {
    if "$@" >"$tmpdir/failure.out" 2>"$tmpdir/failure.err"; then
        echo "expected command to fail: $*" >&2
        exit 1
    fi
}

write_arithmetic_source() {
    cat >"$source_file" <<'MILENA'
funcion principal() {
  variable base = 3;
  variable total = base * 4 + 2;
  total = total - 1;
  retornar total;
}
MILENA
}
write_arithmetic_source

"$cli" --help >"$tmpdir/help"
grep -F 'vm <archivo.milena>' "$tmpdir/help" >/dev/null
grep -F 'build <archivo.milena> -o <programa>' "$tmpdir/help" >/dev/null

"$cli" vm "$source_file" >"$tmpdir/vm.out"
test -s "$tmpdir/vm.out"
if [ -n "$no_native_cli" ]; then
    # The normal product build is native AOT-capable on Linux x86-64.
    # Replacing an existing destination also checks path handling and atomic
    # publication by the native backend.
    printf 'old destination\n' >"$executable"
    "$cli" build "$source_file" -o "$executable"
    test -x "$executable"
    "$executable" >"$tmpdir/native.out"
    cmp "$tmpdir/vm.out" "$tmpdir/native.out"
    rm "$source_file"
    "$executable" >"$tmpdir/without-source.out"
    cmp "$tmpdir/vm.out" "$tmpdir/without-source.out"
    write_arithmetic_source
else
    # On non-Linux-x86-64 products, build must reject the target explicitly and
    # leave an existing destination untouched instead of falling back.
    printf 'sentinel\n' >"$executable"
    expect_failure "$cli" build "$source_file" -o "$executable"
    grep -F 'Linux x86-64' "$tmpdir/failure.err" >/dev/null
    grep -F 'sentinel' "$executable" >/dev/null
fi

expect_failure "$cli" vm "$tmpdir/missing.milena"
grep -F 'No se pudo abrir' "$tmpdir/failure.err" >/dev/null
if [ -n "$no_native_cli" ]; then
    expect_failure "$cli" build "$tmpdir/missing.milena" -o "$tmpdir/missing-program"
    grep -F 'No se pudo abrir' "$tmpdir/failure.err" >/dev/null
fi
expect_failure "$cli" build "$source_file" -o
expect_failure "$cli" build "$source_file" -o "$tmpdir/no-such-dir/program"

cat >"$tmpdir/global.milena" <<'MILENA'
funcion principal() { retornar 1; }
variable extra = 2;
MILENA
# `run` accepts canonical function programs through the AST interpreter; the
# legacy numeric-function parser uses a different return keyword and must not
# intercept this source. The bytecode CLI still rejects the global statement.
"$cli" run "$tmpdir/global.milena" >"$tmpdir/interpreter.out"
expect_failure "$cli" vm "$tmpdir/global.milena"
expect_failure "$cli" build "$tmpdir/global.milena" -o "$tmpdir/unsupported-program"
grep -E -i 'global|sentencias|HIR|soport' "$tmpdir/failure.err" >/dev/null

cat >"$tmpdir/data.milena" <<'MILENA'
.analisis seguridad { #perfil_avanzado("riesgo") }
MILENA
expect_failure "$cli" vm "$tmpdir/data.milena"
expect_failure "$cli" build "$tmpdir/data.milena" -o "$tmpdir/data-program"
grep -E -i 'HIR|nodo|soport|represent' "$tmpdir/failure.err" >/dev/null

if [ -n "$no_native_cli" ]; then
    "$no_native_cli" vm "$tmpdir/data.milena" >"$tmpdir/no-native-vm.out" 2>"$tmpdir/no-native-vm.err" && {
        echo 'unsupported data HIR unexpectedly ran in the bytecode VM' >&2
        exit 1
    }
    expect_failure "$no_native_cli" build "$source_file" -o "$tmpdir/unsupported-target"
    grep -F 'Linux x86-64' "$tmpdir/failure.err" >/dev/null
    test ! -e "$tmpdir/unsupported-target"
fi

cat >"$tmpdir/bad-syntax.milena" <<'MILENA'
funcion principal( { retornar 1; }
MILENA
expect_failure "$cli" vm "$tmpdir/bad-syntax.milena"

echo 'bytecode CLI integration tests: VM, native build/run, source-independent output, parity, fail-closed subset, unsupported target, and I/O errors OK'
