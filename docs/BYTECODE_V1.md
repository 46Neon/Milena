# Portable MLBC v1 — experimental typed-verifier slice

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
record are unchanged. v1.2 adds an explicit, bounded type-table trailer without
changing either legacy representation. All integers are unsigned, fixed-width,
and little-endian. The header is `MLBC`, major 1, minor 0, 1, or 2, register
count (1–256), zero reserved bits, and instruction count (1–65,536). The
instruction record stores opcode, three zero reserved bytes, operands `a`, `b`,
`c`, and an 8-byte immediate. v1.0/v1.1 length remains exactly
`16 + instruction_count * 24`; v1.2 appends exactly `register_count` type bytes
followed by one return-type byte for every function marker, in function-ID order.
Truncation, excess metadata, and trailing bytes are rejected. Numeric constants
use IEEE-754 binary64 bit patterns.

v1.0 retains the original opcodes and single-entry semantics. v1.1 adds:

| Opcode | Operand convention |
|---|---|
| `FUNCTION` | `a=function_id`, `b=parameter_register_base`, `c=arity`, immediate zero. It begins a function region; the first marker is ID 0, IDs are consecutive, and the body starts at the next instruction. |
| `CALL` | `a=destination_register`, `b=target_function_id`, `c=argument_register_base`, immediate is an exactly integral numeric arity. Arguments and parameters occupy consecutive register ranges. |
| `CONST_BOOL` (v1.2) | `a=destination_register`; immediate is exactly binary64 `0.0` or `1.0`; `b=c=0`. |


### v1.2 static type contract

Only v1.2 carries type guarantees. Its trailer contains a type tag for every
register (`1=NUMBER`, `2=BOOLEAN`) followed by each function's declared return
type in ascending function-ID order. The parameter types of a function are the
tags of the consecutive registers named by its `FUNCTION` marker. This fixed,
program-wide register typing is the merge contract: a register cannot change
type on different control-flow paths, and every write/opcode is checked against
its declared tag. It deliberately avoids unbounded or path-sensitive type-flow
analysis; incompatible branch writes fail at the writing instruction.

Before any v1.2 execution, the verifier rejects unknown/malformed type tags or
wrong table lengths; requires finite `CONST_F64` values in numeric registers and
`CONST_BOOL` values in boolean registers; enforces same-type `MOVE`; numeric
operands/results for arithmetic and negation; numeric comparison operands and
boolean comparison results (with `EQ`/`NE` also permitting two booleans); boolean
conditional-jump operands; and `RETURN` against that function's declared return
type. `CALL` arguments must match the callee's parameter-register tags and its
destination must match the callee return type. The entry function is required
to return a number because the current public VM/AOT result APIs expose a
`double`, not a typed result. Boolean-returning helpers are valid. Verification
remains bounded by the existing instruction/register/function caps and fails
closed; runtime re-verifies the exact serialized bytes.

v1.0/v1.1 deliberately retain their original, untyped semantics for wire
compatibility. In those formats comparisons and booleans may be represented by
numeric zero/nonzero values, and this verifier does **not** claim static type
safety for them. Existing v1.0/v1.1 fixtures, call behavior, and VM execution
remain tested. The canonical source-to-bytecode compiler currently emits v1.1
and therefore does not yet emit the v1.2 type table or provide v1.2 guarantees;
manual/other producers can use the v1.2 API by supplying both exact metadata
tables.

The function marker table is encoded in the same instruction stream, rather than
out-of-band metadata. Function IDs, parameter and argument ranges, exact arity,
register operands, targets, branch ownership, and per-function control flow are
validated before execution. Branches cannot cross function boundaries. Each
function must have a reachable return matching its declared type in v1.2 (the
legacy v1.0/v1.1 forms return numbers). Calls are statically checked to form a
directed acyclic graph: direct and mutual recursion are deliberately rejected
in v1.1+, not left to consume unbounded host stack. There are at most 64
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
before runtime. v1.0/v1.1 intentionally do not implement definite-assignment or
static value-type analysis. v1.2 adds the explicit fixed-register type contract
described above; it is not a claim of general language type-flow analysis.

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

Focused differential tests cover `principal -> combinar(doble(5), 3)` against
the canonical interpreter, the bytecode VM, and the native executable. The name
`combinar` is an ordinary user-function identifier; `suma` is reserved by the
lexer for a statistical operation and is not a valid user-function identifier
in this grammar. Additional coverage includes forward/helper functions, unused
helper declarations, recursive-graph rejection, malformed function IDs and
arities, semantic wrong arity, callee runtime errors, frame/fuel exhaustion,
and successful later runs after bounded-runtime failures. A source program
with top-level output binding is used as the canonical interpreter comparison
wrapper.

## Native AOT backend

`milena_bytecode_compile_native` verifies the same serialized bytes before it
creates an output artifact. On Linux x86-64 with IEEE-754 binary64, v1.0 is
emitted as direct C labels/gotos. For v1.1/v1.2, it emits a C function per
encoded function and emits actual direct calls between those generated functions
using the same encoded function IDs, parameter ranges, and call sites; there is
no VM dispatch/interpreter loop. v1.2 type metadata is verified before emission,
and `CONST_BOOL` is emitted as a constant assignment. C functions share the
source-specific register file and fuel counter, snapshot incoming arguments, and propagate numeric/runtime
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
remain out of scope. `make test-bytecode` runs v1.0/v1.1 compatibility tests,
v1.2 static-type verifier/VM/AOT tests, canonical lowering, differential,
native-call, malformed-input, runtime-error, limit, cleanup, and sanitizer-ready
regression suites.
