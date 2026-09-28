#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MILENA="$ROOT/milena"
FIXTURES="$ROOT/tests/fixtures/native_aot"
TEMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/milena-native-aot-XXXXXX")
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

"$MILENA" --help | grep -F 'build <archivo.milena> -o <ejecutable>' >/dev/null
"$MILENA" build "$FIXTURES/branches_calls.milena" -o "$TEMP_DIR/branches"
[ -x "$TEMP_DIR/branches" ]
[ "$("$TEMP_DIR/branches")" = '42' ]

"$MILENA" build "$FIXTURES/division_by_zero.milena" -o "$TEMP_DIR/division-by-zero"
set +e
"$TEMP_DIR/division-by-zero" >"$TEMP_DIR/runtime.out" 2>"$TEMP_DIR/runtime.err"
RUNTIME_STATUS=$?
set -e
[ "$RUNTIME_STATUS" -eq 70 ]
grep -F 'division by zero' "$TEMP_DIR/runtime.err" >/dev/null

"$MILENA" build "$FIXTURES/non_finite.milena" -o "$TEMP_DIR/non-finite"
set +e
"$TEMP_DIR/non-finite" >"$TEMP_DIR/non-finite.out" 2>"$TEMP_DIR/non-finite.err"
NON_FINITE_STATUS=$?
set -e
[ "$NON_FINITE_STATUS" -eq 70 ]
grep -F 'non-finite numeric result' "$TEMP_DIR/non-finite.err" >/dev/null

printf 'keep-existing-output\n' >"$TEMP_DIR/preserved"
if MILENA_CC=/milena-test-no-such-c-compiler \
    "$MILENA" build "$FIXTURES/branches_calls.milena" -o "$TEMP_DIR/preserved" \
    >"$TEMP_DIR/compiler.out" 2>"$TEMP_DIR/compiler.err"; then
    echo 'AOT unexpectedly succeeded without a C compiler' >&2
    exit 1
fi
[ "$(cat "$TEMP_DIR/preserved")" = 'keep-existing-output' ]

if "$MILENA" build "$FIXTURES/no_principal.milena" -o "$TEMP_DIR/preserved" \
    >"$TEMP_DIR/reject.out" 2>"$TEMP_DIR/reject.err"; then
    echo 'AOT unexpectedly accepted a source without principal()' >&2
    exit 1
fi
[ "$(cat "$TEMP_DIR/preserved")" = 'keep-existing-output' ]

cp "$FIXTURES/branches_calls.milena" "$TEMP_DIR/alias.milena"
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
if find "$TEMP_DIR" -name '.milena-aot-*' -print | grep .; then
    echo 'AOT left temporary files behind' >&2
    exit 1
fi

echo 'Native AOT CLI tests passed.'
