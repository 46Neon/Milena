# Portable MLBC v1 — experimental function-call slice

This document specifies the isolated bytecode/verifier/VM implementation in
`include/bytecode.h` and `src/bytecode.c`, canonical scalar-HIR lowering in
`src/bytecode_compiler.c`, and the optional Linux x86-64 native backend in
`src/bytecode_native.c`. These experimental modules remain outside the shipped
production source list. The PR remains partial: compiler-plan phase 2 and full
phase 3 are not complete, and this slice is not production compiler integration.

## Wire compatibility and records

The original v1.0 format remains unchanged and is still accepted by its verifier,
VM, and native AOT tests. Function calls are introduced as an intentional v1.1
minor-version extension; the 16-byte header and 24-byte fixed-width instruction
record are unchanged. All integers are unsigned, fixed-width, and little-endian.
The header is `MLBC`, major 1, minor 0 or 1, register count (1–256), zero reserved
bits, and instruction count (1–65,536). The instruction record stores opcode,
three zero reserved bytes, operands `a`, `b`, `c`, and an 8-byte immediate. The
length must equal `16 + instruction_count * 24`; truncation and trailing bytes
are rejected. Numeric constants use IEEE-754 binary64 bit patterns.

v1.0 retains the original opcodes and single-entry semantics. v1.1 adds:

| Opcode | Operand convention |
|---|---|
| `FUNCTION` | `a=function_id`, `b=parameter_register_base`, `c=arity`, immediate zero. It begins a function region; the first marker is ID 0, IDs are consecutive, and the body starts at the next instruction. |
| `CALL` | `a=destination_register`, `b=target_function_id`, `c=argument_register_base`, immediate is an exactly integral numeric arity. Arguments and parameters occupy consecutive register ranges. |

The function marker table is encoded in the same instruction stream, rather than
out-of-band metadata. Function IDs, parameter and argument ranges, exact arity,
register operands, targets, branch ownership, and per-function control flow are
validated before execution. Branches cannot cross function boundaries. Each
function must have a reachable numeric return. Calls are statically checked to
form a directed acyclic graph: direct and mutual recursion are deliberately
rejected in v1.1, not left to consume unbounded host stack. There are at most 64
functions and the hard call-frame limit is 64; callers may lower the VM frame
limit. The runtime checks the frame limit before pushing a frame, snapshots
arguments before copying them to the callee parameter registers, and unwinds all
allocated run state on errors. Repeated calls safely reuse the callees' disjoint
static register ranges; the call graph's non-recursive restriction is part of
that register-allocation contract.

The VM starts at function ID 0. It maintains one zero-initialized bounded
register file, a bounded explicit return stack, and a shared instruction-fuel
budget. The default budget is 1,000,000 instructions and the hard ceiling is
10,000,000. Division by zero, non-finite arithmetic, malformed bytecode, frame
exhaustion, and fuel exhaustion return explicit status codes and a zero result.
The verifier uses bounded temporary workspaces and rejects recursive graphs
before runtime. It intentionally does not implement definite-assignment or
static value-type analysis; the source compiler provides initialized locals for
its closed subset.

## Canonical source lowering

`milena_bytecode_compile_source` passes source through the official canonical
lexer/parser/semantic/name-resolution path, obtains `MilenaScalarHIR`, and lowers
only that resolved HIR. It does not bypass semantic resolution or silently
ignore statements. The supported source subset consists of one zero-parameter
`principal` and up to 63 additional scalar numeric helper functions, including
forward declarations/calls and multiple calls. Each lowered function must
return a number along every path. Numeric parameters, initialized numeric or
boolean locals, assignments, finite literals, numeric `+ - * /`, same-type
numeric comparisons, boolean equality/inequality, nested `si`/`sino`, and
resolved numeric function calls are supported. Call arguments and returns use
numeric values; calls with boolean ABI values are rejected without coercion.
All globals, unsupported HIR/statement forms, mixed-type or boolean-relational
comparisons, unreachable statements after a guaranteed return, malformed
bindings, missing returns, register/function/instruction-cap overflow, wrong
arity, and recursive or mutually recursive call graphs are rejected. Function
calls continue through semantic resolution and the compiler uses resolved
symbol IDs to link canonical HIR declarations.

The compiler reserves disjoint static register ranges per function and emits a
contiguous temporary argument range at each call site. This is bounded by 256
total registers. Function ID zero is `principal`; other IDs follow source order.
The bytecode encoder verifies the complete emitted v1.1 stream before publishing
it. `milena_bytecode_run` verifies it again before allocating the runtime state.

Focused differential tests cover `principal -> suma(doble(5), 3)` against the
canonical interpreter, the bytecode VM, and the native executable. Additional
coverage includes forward/helper functions, unused helper declarations,
recursive-graph rejection, malformed function IDs and arities, semantic wrong
arity, callee runtime errors, frame/fuel exhaustion, and successful later runs
after bounded-runtime failures. A source program with top-level output binding
is used as the canonical interpreter comparison wrapper.

## Native AOT backend

`milena_bytecode_compile_native` verifies the same serialized bytes before it
creates an output artifact. On Linux x86-64 with IEEE-754 binary64, v1.0 is
emitted as direct C labels/gotos. For v1.1, it emits a C function per encoded
function and emits actual direct calls between those generated functions using
the same encoded function IDs, parameter ranges, and call sites; there is no VM
dispatch/interpreter loop. C functions share the source-specific register file
and fuel counter, snapshot incoming arguments, and propagate numeric/runtime
status. The verifier's non-recursive graph bound prevents unbounded native call
stack growth. Native runtime errors exit 70, fuel exhaustion exits 71, and stdout
failure exits 74. The backend invokes fixed `/usr/bin/cc` via `fork`/`execve`
with an argument vector, uses private temporary files, and atomically publishes
the executable only on success. Native artifacts print the result as a C
hexadecimal floating literal.

The optional AOT path remains limited to Linux x86-64; the API uses its fixed
1,000,000-step budget. It is not the compiler-plan AOT gate. Windows, Android /
Termux, general parameterized entry points, recursion, broader language parity,
data-HIR lowering, CLI integration, and production source-manifest inclusion
remain out of scope. `make test-bytecode` runs the v1.0 compatibility tests and
the v1.1 verifier, canonical lowering, VM, differential, native-call, malformed
input, runtime-error, limit, cleanup, and sanitizer-ready regression suites.
