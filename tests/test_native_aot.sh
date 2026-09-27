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

check_runtime_case() {
    fixture=$1
    name=$2
    opcode_name=$3
    lhs=$4
    rhs=$5
    opcode=$6
    expected=$7
    if [ "$name" = add ]; then
        MILENA_AOT_COMPILER_INVOKED="$TEMP_DIR/compiler-invoked" \
        MILENA_CC="$TEMP_DIR/forbidden-compiler" \
            "$MILENA" build "$FIXTURES/$fixture" -o "$TEMP_DIR/$name"
        [ ! -e "$TEMP_DIR/compiler-invoked" ]
    else
        "$MILENA" build "$FIXTURES/$fixture" -o "$TEMP_DIR/$name"
    fi
    [ -x "$TEMP_DIR/$name" ]
    file "$TEMP_DIR/$name" | grep -F 'ELF 64-bit LSB executable, x86-64' >/dev/null
    # Check the actual generated code section, not constants/data in the ELF.
    python3 - "$TEMP_DIR/$name" "$opcode" <<'PYCODE'
import pathlib, sys
image = pathlib.Path(sys.argv[1]).read_bytes()
code = image[120:]
needle = bytes.fromhex(sys.argv[2])
if needle not in code:
    raise SystemExit(f'missing emitted arithmetic opcode: {needle.hex()}')
PYCODE
    "$ROOT/tests/test_typed_bytecode" --aot-reference "$opcode_name" "$lhs" "$rhs" \
        >"$TEMP_DIR/$name.vm.out" 2>"$TEMP_DIR/$name.vm.err"
    "$TEMP_DIR/$name" >"$TEMP_DIR/$name.native.out" 2>"$TEMP_DIR/$name.native.err"
    cmp "$TEMP_DIR/$name.vm.out" "$TEMP_DIR/$name.native.out"
    [ "$(cat "$TEMP_DIR/$name.native.out")" = "$expected" ]
}

# Runtime arithmetic must be in the executable: each opcode is asserted in the
# RX text bytes, the ELF is run, and its result is compared with verified bytecode VM execution.
check_runtime_case constant_arithmetic.milena add add 19 23 'f2 0f 58 c1' 42
check_runtime_case subtraction.milena sub sub 0 42 'f2 0f 5c c1' -42
check_runtime_case multiplication.milena mul mul 6 7 'f2 0f 59 c1' 42
check_runtime_case division.milena div div 84 2 'f2 0f 5e c1' 42

# The verifier keeps the supported runtime-error behavior fail-closed and
# checks its diagnostic against the typed-IR reference VM's error case.
"$MILENA" build "$FIXTURES/division_by_zero.milena" -o "$TEMP_DIR/division-by-zero"
set +e
"$ROOT/tests/test_typed_bytecode" --aot-reference div 1 0 \
    >"$TEMP_DIR/division.vm.out" 2>"$TEMP_DIR/division.vm.err"
VM_DIVISION_STATUS=$?
"$TEMP_DIR/division-by-zero" >"$TEMP_DIR/division.out" 2>"$TEMP_DIR/division.err"
DIVISION_STATUS=$?
set -e
[ "$VM_DIVISION_STATUS" -eq 70 ]
[ "$DIVISION_STATUS" -eq 70 ]
grep -F 'division by zero' "$TEMP_DIR/division.vm.err" >/dev/null
grep -F 'Milena native runtime error: division by zero' "$TEMP_DIR/division.err" >/dev/null

# Out-of-slice floating arithmetic is rejected without replacing an existing output.
printf 'keep-existing-output\n' >"$TEMP_DIR/non-finite"
if "$MILENA" build "$FIXTURES/non_finite.milena" -o "$TEMP_DIR/non-finite" \
    >"$TEMP_DIR/non-finite.out" 2>"$TEMP_DIR/non-finite.err"; then
    echo 'AOT unexpectedly accepted non-finite/out-of-range arithmetic' >&2
    exit 1
fi
grep -E 'subconjunto entero exacto|subconjunto ELF directo' "$TEMP_DIR/non-finite.err" >/dev/null
[ "$(cat "$TEMP_DIR/non-finite")" = 'keep-existing-output' ]

# Direct helper calls execute as x86-64 CALL rel32 instructions using SysV
# floating-point arguments/results. Compare the helper-call result to the
# typed-bytecode reference VM, not to a compile-time folded constant.
"$MILENA" build "$FIXTURES/direct_calls.milena" -o "$TEMP_DIR/direct-calls"
[ -x "$TEMP_DIR/direct-calls" ]
python3 - "$TEMP_DIR/direct-calls" <<'PYCODE'
import pathlib, sys
code = pathlib.Path(sys.argv[1]).read_bytes()[120:]
if code.count(b'\xe8') < 2:
    raise SystemExit('expected emitted entry/helper direct CALL rel32 instructions')
PYCODE
"$ROOT/tests/test_typed_bytecode" --aot-reference-calls >"$TEMP_DIR/calls.vm.out" 2>"$TEMP_DIR/calls.vm.err"
"$TEMP_DIR/direct-calls" >"$TEMP_DIR/calls.native.out" 2>"$TEMP_DIR/calls.native.err"
cmp "$TEMP_DIR/calls.vm.out" "$TEMP_DIR/calls.native.out"
[ "$(cat "$TEMP_DIR/calls.native.out")" = '6' ]

# A helper's native division-by-zero behavior must match the typed-bytecode VM.
"$MILENA" build "$FIXTURES/direct_call_division_by_zero.milena" -o "$TEMP_DIR/call-div-zero"
set +e
"$ROOT/tests/test_typed_bytecode" --aot-reference-call-div-zero >"$TEMP_DIR/call-div.vm.out" 2>"$TEMP_DIR/call-div.vm.err"
CALL_VM_STATUS=$?
"$TEMP_DIR/call-div-zero" >"$TEMP_DIR/call-div.native.out" 2>"$TEMP_DIR/call-div.native.err"
CALL_NATIVE_STATUS=$?
set -e
[ "$CALL_VM_STATUS" -eq 70 ]
[ "$CALL_NATIVE_STATUS" -eq 70 ]
grep -F 'division by zero' "$TEMP_DIR/call-div.vm.err" >/dev/null
grep -F 'Milena native runtime error: division by zero' "$TEMP_DIR/call-div.native.err" >/dev/null

# SysV floating-point calls beyond the eight XMM argument registers are rejected.
printf 'keep-existing-output\n' >"$TEMP_DIR/preserved"
if "$MILENA" build "$FIXTURES/too_many_arguments.milena" -o "$TEMP_DIR/preserved" \
    >"$TEMP_DIR/too-many-args.out" 2>"$TEMP_DIR/too-many-args.err"; then
    echo 'AOT unexpectedly accepted more than eight direct-call arguments' >&2
    exit 1
fi
grep -F 'máximo 8 argumentos F64' "$TEMP_DIR/too-many-args.err" >/dev/null
[ "$(cat "$TEMP_DIR/preserved")" = 'keep-existing-output' ]

# Control flow outside the small verified slice fails closed and preserves output.
printf 'keep-existing-output\n' >"$TEMP_DIR/preserved"
if "$MILENA" build "$FIXTURES/branches_calls.milena" -o "$TEMP_DIR/preserved" \
    >"$TEMP_DIR/reject.out" 2>"$TEMP_DIR/reject.err"; then
    echo 'AOT unexpectedly accepted unsupported control flow/calls' >&2
    exit 1
fi
grep -E 'subconjunto AOT de un bloque|subconjunto ELF directo|AOT directo requiere' "$TEMP_DIR/reject.err" >/dev/null
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
