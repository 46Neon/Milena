# Portable bytecode v1 — experimental slice

This document specifies the small, versioned bytecode/verifier/VM slice in
`include/bytecode.h` and `src/bytecode.c`, its experimental canonical scalar-HIR
lowering in `src/bytecode_compiler.c`, and an optional native backend in
`src/bytecode_native.c`. The wire format remains portable serialized bytecode;
it is **not native machine code**. The native backend is a separate, explicitly
limited Linux x86-64 AOT path over the exact same verified MLBC bytes executed
by the VM. Neither module is called by the shipped CLI/runtime, and they remain
outside the production `SOURCES` list.

## Wire format

All integers are unsigned, fixed-width, and little-endian. The header is exactly
16 bytes:

| Offset | Width | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII magic `MLBC` |
| 4 | 2 | Major version (1) |
| 6 | 2 | Minor version (0) |
| 8 | 2 | Register count (1–256) |
| 10 | 2 | Reserved; must be zero |
| 12 | 4 | Instruction count (1–65,536) |

Each instruction is exactly 24 bytes:

| Offset | Width | Meaning |
|---:|---:|---|
| 0 | 1 | Opcode |
| 1 | 3 | Reserved; must be zero |
| 4 | 4 | Operand `a` |
| 8 | 4 | Operand `b` |
| 12 | 4 | Operand `c` |
| 16 | 8 | Immediate |

The bytecode length must equal `16 + instruction_count * 24`; truncated and
trailing data are rejected. Unused operands, reserved bytes, and non-constant
immediates must be zero. `CONST_F64` stores an IEEE-754 binary64 value as its
64-bit bit pattern. Hosts without binary64 support reject this bytecode rather
than reinterpret it. The format has no host pointers, native instructions,
strings, or implicit host-endian fields.

## Instructions and execution

The initial opcode set is `CONST_F64`, `MOVE`, `ADD`, `SUB`, `MUL`, `DIV`,
`NEG`, `EQ`, `NE`, `LT`, `LE`, `GT`, `GE`, `JUMP`, `JUMP_IF_FALSE`, and
`RETURN`. A binary arithmetic/comparison instruction uses `a` as destination
and `b`/`c` as sources; unary operations use `a` as destination and `b` as
source. `JUMP` uses `a` as an instruction index. `JUMP_IF_FALSE` uses `a` as
a register and `b` as the target; zero branches, nonzero falls through.
`RETURN` uses `a` as the returned numeric register. Comparisons write `1.0` or
`0.0`.

Execution is a single-entry numeric function with a fixed register file. At the
start of each call all registers are initialized to `0.0`; the VM allocates and
frees its register file for that call. Arithmetic that produces a non-finite
value, division by zero, malformed code, or exhausted instruction budget
returns an explicit error and a zero result. The default execution budget is
1,000,000 instructions; callers may lower it, but not raise the hard cap of
10,000,000. Configurable byte-size, instruction-count, and register-count limits
are likewise clamped to compile-time hard caps.

Before execution, the complete byte stream is revalidated. The verifier checks
version/length/reserved bits/opcodes/operand ranges, finite constants, valid
branch targets, reachable fallthrough, and the existence of a reachable return.
The VM additionally checks the dynamic step budget and numeric operations. The
verifier uses bounded temporary control-flow workspaces and releases them on
success and failure. This first slice does not do definite-assignment/type
analysis; zero-initialized registers are part of its current execution
contract.

## Experimental source-to-bytecode path

`include/bytecode_compiler.h` exposes `milena_bytecode_compile_source`. It
passes source through the official `milena_canonical_program_parse` route
(lexer, parser, AST validation, semantic/name resolution, and owned scalar
HIR), obtains the fail-closed canonical compiler input, and lowers only that
validated `MilenaScalarHIR`; it does not inspect or lower the AST directly.
Success returns a caller-owned encoded byte array. Failure clears both output
fields and reports `MilenaError` with the original line/column when available.
The normal bytecode encoder validates the resulting program before it is
published, and `milena_bytecode_run` verifies it again before execution.

The intentionally closed source subset is one function named `principal`,
with zero parameters and no top-level statements or additional functions. Its
body supports initialized numeric or boolean locals, assignment to those
bindings, finite numeric/boolean literals, numeric `+ - * /`, same-type numeric
comparisons, boolean `==`/`!=`, nested `si`/`sino` branches, and numeric
`retornar`. Conditions accept the canonical scalar number/boolean forms and
follow the existing VM contract (zero is false, nonzero is true). Every path
must return a number. Calls (including their arguments), mixed-type comparisons,
boolean relational comparisons, unsupported HIR variants, missing bindings,
unreachable statements after a guaranteed return, and resource-cap overflow
are rejected with a source diagnostic; no statement is silently discarded and
no implicit coercion or fallback is performed. Lowering uses resolved HIR
binding IDs, a bounded register mapping, and absolute instruction-index branch
patching. The entry function's result is not whole-program equivalence: tests
compare it with the legacy interpreter executing the corresponding function
plus a top-level `variable salida = principal();` wrapper.

This remains an isolated experimental foundation rather than production
compiler integration. It has no function-call ABI, data HIR lowering, strings,
datasets, or typed data runtime, and it does not directly lower HIR to native
code: AOT consumes only already-verified MLBC bytes. It does not replace the
experimental legacy `src/vm.c`, and the bytecode/compiler/AOT modules remain
outside production `SOURCES`; this slice does not establish parity for all
`.milena` constructions or mark compiler-plan phase 2, Phase 3, Phase 4, or
Phase 6 complete.

## Experimental native AOT for the closed subset

`milena_bytecode_compile_native` in `include/bytecode_compiler.h` first verifies
the complete byte stream and refuses malformed input before creating an output.
It currently supports Linux x86-64 with IEEE-754 binary64 only. It decodes the
verified instructions and emits per-instruction C labels and direct `goto`
control flow, then invokes the fixed `/usr/bin/cc` using `fork`/`execve` and an
argument vector; there is no shell command construction, user-bytecode text is
never interpolated, and the generated artifact has no bytecode dispatch loop.
The generated code has source-specific native arithmetic/comparison/branch
instructions, a zero-initialized register file, finite-result and divide-by-zero
checks, and the VM's default 1,000,000-instruction budget. Private temporary
files are cleaned on all paths and the final executable is atomically published
only after successful compilation. Compiler diagnostics remain on stderr and
API errors report verification, I/O, and compiler failures.

The artifact prints a successful numeric result as a C hexadecimal floating
literal on stdout and exits 0. It exits 70 for VM runtime errors, 71 for the
instruction budget, and 74 if result output fails. AOT is deliberately limited
to the VM's default fuel value (unlike `milena_bytecode_run`, callers cannot
supply a custom fuel budget to the artifact). The focused source-compiler smoke
tests compile the same source-derived, verified MLBC bytes to AOT and compare
both native numeric output and process success with `milena_bytecode_run` for
arithmetic and both branch outcomes. Additional tests compare native runtime
error and fuel-exhaustion exits with VM statuses, reject invalid bytecode before
output publication, and use shell metacharacters in an output path to exercise
argv-only invocation. The bytecode CI jobs run this test on Linux x86-64 with
GCC/Clang harness builds and under ASan/UBSan; the generated native child is
compiled with the fixed system compiler.

This is not the compiler-plan AOT gate: it validates one numeric/boolean
`principal` subset and one Linux target only. Windows PE, Android/Termux Bionic,
ABI/call lowering, HIR/data lowering, broad semantic parity, CLI integration,
production source-manifest inclusion, and the remaining compiler plan gates
are still open. `make test-bytecode` runs the verifier/VM, canonical HIR lowering,
and native-artifact regression suites; all experimental modules remain
excluded from production `SOURCES`.
