#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MILENA="$ROOT/milena"
FIXTURES="$ROOT/tests/fixtures/native_aot"
TEMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/milena-native-aot-XXXXXX")
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

"$MILENA" --help | grep -F 'build <archivo.milena> -o <ejecutable>' >/dev/null

# The compiler must not delegate AOT to MILENA_CC (or any other C compiler).
cat >"$TEMP_DIR/forbidden-compiler" <<'EOF'
#!/bin/sh
: >"$MILENA_AOT_COMPILER_INVOKED"
exit 99
EOF
chmod +x "$TEMP_DIR/forbidden-compiler"
MILENA_AOT_COMPILER_INVOKED="$TEMP_DIR/compiler-invoked" \
MILENA_CC="$TEMP_DIR/forbidden-compiler" \
    "$MILENA" build "$FIXTURES/constant_arithmetic.milena" -o "$TEMP_DIR/constant"
[ ! -e "$TEMP_DIR/compiler-invoked" ]
[ -x "$TEMP_DIR/constant" ]
file "$TEMP_DIR/constant" | grep -F 'ELF 64-bit LSB executable, x86-64' >/dev/null

# Differential check against the verified-bytecode reference VM for the same
# constant typed-IR operations used by the source fixture.
"$ROOT/tests/test_typed_bytecode" --aot-reference >"$TEMP_DIR/vm.out"
"$TEMP_DIR/constant" >"$TEMP_DIR/native.out" 2>"$TEMP_DIR/native.err"
cmp "$TEMP_DIR/vm.out" "$TEMP_DIR/native.out"
[ "$(cat "$TEMP_DIR/native.out")" = '42' ]

# Failure forms have explicit native diagnostics and deterministic exit status.
"$MILENA" build "$FIXTURES/division_by_zero.milena" -o "$TEMP_DIR/division-by-zero"
set +e
"$TEMP_DIR/division-by-zero" >"$TEMP_DIR/runtime.out" 2>"$TEMP_DIR/runtime.err"
DIVISION_STATUS=$?
set -e
[ "$DIVISION_STATUS" -eq 70 ]
grep -F 'Milena native runtime error: division by zero' "$TEMP_DIR/runtime.err" >/dev/null

"$MILENA" build "$FIXTURES/non_finite.milena" -o "$TEMP_DIR/non-finite"
set +e
"$TEMP_DIR/non-finite" >"$TEMP_DIR/non-finite.out" 2>"$TEMP_DIR/non-finite.err"
NON_FINITE_STATUS=$?
set -e
[ "$NON_FINITE_STATUS" -eq 70 ]
grep -F 'Milena native runtime error: non-finite numeric result' "$TEMP_DIR/non-finite.err" >/dev/null

# Control flow/calls outside the small verified slice fail closed and preserve output.
printf 'keep-existing-output\n' >"$TEMP_DIR/preserved"
if "$MILENA" build "$FIXTURES/branches_calls.milena" -o "$TEMP_DIR/preserved" \
    >"$TEMP_DIR/reject.out" 2>"$TEMP_DIR/reject.err"; then
    echo 'AOT unexpectedly accepted unsupported control flow/calls' >&2
    exit 1
fi
grep -E 'AOT directo requiere|subconjunto ELF directo' "$TEMP_DIR/reject.err" >/dev/null
[ "$(cat "$TEMP_DIR/preserved")" = 'keep-existing-output' ]

if "$MILENA" build "$FIXTURES/no_principal.milena" -o "$TEMP_DIR/preserved" \
    >"$TEMP_DIR/no-entry.out" 2>"$TEMP_DIR/no-entry.err"; then
    echo 'AOT unexpectedly accepted a source without principal()' >&2
    exit 1
fi
[ "$(cat "$TEMP_DIR/preserved")" = 'keep-existing-output' ]

cp "$FIXTURES/constant_arithmetic.milena" "$TEMP_DIR/alias.milena"
cp "$TEMP_DIR/alias.milena" "$TEMP_DIR/alias.expected"
if "$MILENA" build "$TEMP_DIR/alias.milena" -o "$TEMP_DIR/alias.milena" \
    >"$TEMP_DIR/alias.out" 2>"$TEMP_DIR/alias.err"; then
    echo 'AOT unexpectedly accepted source/output aliasing' >&2
    exit 1
fi
cmp "$TEMP_DIR/alias.expected" "$TEMP_DIR/alias.milena"
ln "$TEMP_DIR/alias.milena" "$TEMP_DIR/alias.hardlink"
if "$MILENA" build "$TEMP_DIR/alias.milena" -o "$TEMP_DIR/alias.hardlink" \
    >"$TEMP_DIR/hardlink.out" 2>"$TEMP_DIR/hardlink.err"; then
    echo 'AOT unexpectedly accepted a hard-link source/output alias' >&2
    exit 1
fi
ln -s "$TEMP_DIR/alias.milena" "$TEMP_DIR/alias.symlink"
if "$MILENA" build "$TEMP_DIR/alias.milena" -o "$TEMP_DIR/alias.symlink" \
    >"$TEMP_DIR/symlink.out" 2>"$TEMP_DIR/symlink.err"; then
    echo 'AOT unexpectedly accepted a symlink source/output alias' >&2
    exit 1
fi
cmp "$TEMP_DIR/alias.expected" "$TEMP_DIR/alias.milena"
if find "$TEMP_DIR" -name '.milena-elf-*' -print | grep .; then
    echo 'AOT left temporary files behind' >&2
    exit 1
fi

echo 'Direct native ELF AOT tests passed.'
