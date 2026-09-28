#!/bin/sh
set -eu

cli=${1:-./milena}
no_native_cli=${2:-}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/milena-bytecode-data-cli.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

printf 'valor\n1.5\n2.5\n\n' >"$tmpdir/data.csv"

expect_failure() {
    if "$@" >"$tmpdir/failure.out" 2>"$tmpdir/failure.err"; then
        echo "expected command to fail: $*" >&2
        exit 1
    fi
}

write_summary_source() {
    summary_name=$1
    summary_operation=$2
    summary_output=$3
    cat >"$tmpdir/$summary_name.milena" <<EOF
.analisis resumen {
  variable valor numerica
  dataset cargar datos("data.csv")
  .resumir dataset { #$summary_operation("valor") }
  .exportar { ("$summary_output") }
}
EOF
}

run_differential_case() {
    name=$1
    operation=$2
    expected=$3
    write_summary_source "$name-run" "$operation" "$name-run.json"
    write_summary_source "$name-vm" "$operation" "$name-vm.json"

    "$cli" run "$tmpdir/$name-run.milena" >"$tmpdir/$name-run.stdout"
    "$cli" vm "$tmpdir/$name-vm.milena" >"$tmpdir/$name-vm.stdout"
    test ! -s "$tmpdir/$name-vm.stdout"
    cmp "$tmpdir/$name-run.json" "$tmpdir/$name-vm.json"
    grep -F "$expected" "$tmpdir/$name-run.json" >/dev/null

    # Exercise the same verified v1.3 VM on the no-native CLI build as well.
    if [ -n "$no_native_cli" ]; then
        write_summary_source "$name-no-native" "$operation" "$name-no-native.json"
        "$no_native_cli" vm "$tmpdir/$name-no-native.milena" \
            >"$tmpdir/$name-no-native.stdout"
        test ! -s "$tmpdir/$name-no-native.stdout"
        cmp "$tmpdir/$name-run.json" "$tmpdir/$name-no-native.json"
    fi
}

run_differential_case sum suma '"valor_suma":4'
run_differential_case count conteo '"valor_conteo":2'

# Data-only v1.3 modules are intentionally VM-only; native build must reject
# them before publishing or replacing an executable.
write_summary_source build-data suma build-data-run.json
printf 'sentinel\n' >"$tmpdir/build-output"
expect_failure "$cli" build "$tmpdir/build-data.milena" -o "$tmpdir/build-output"
test "$(cat "$tmpdir/build-output")" = sentinel
if [ -n "$no_native_cli" ]; then
    grep -F 'no admite módulos MLBC v1.3 de solo datos' "$tmpdir/failure.err" >/dev/null
    expect_failure "$no_native_cli" build "$tmpdir/build-data.milena" \
        -o "$tmpdir/no-native-build-output"
    test ! -e "$tmpdir/no-native-build-output"
fi

# Streaming data HIR has a valid canonical parse but is outside the exact v1.3
# plan shape. It must fail during compilation and never create its export.
cat >"$tmpdir/stream.milena" <<'MILENA'
.analisis resumen {
  variable valor numerica
  dataset cargar flujo("data.csv", 2)
  .resumir dataset { #suma("valor") }
  .exportar { ("stream-vm.json") }
}
MILENA
expect_failure "$cli" vm "$tmpdir/stream.milena"
test ! -e "$tmpdir/stream-vm.json"

# A second HIR operation is likewise rejected before execution/publication.
cat >"$tmpdir/extra-hir.milena" <<'MILENA'
.analisis resumen {
  variable valor numerica
  dataset cargar datos("data.csv")
  .resumir dataset { #suma("valor") }
  .resumir dataset { #conteo("valor") }
  .exportar { ("extra-hir-vm.json") }
}
MILENA
expect_failure "$cli" vm "$tmpdir/extra-hir.milena"
test ! -e "$tmpdir/extra-hir-vm.json"

echo 'bytecode data CLI integration tests: canonical SUM/COUNT JSON parity, source-relative paths, native AOT rejection, and fail-closed streaming/extra-HIR cases OK'
