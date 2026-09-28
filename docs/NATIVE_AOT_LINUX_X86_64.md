# Direct native AOT slice: Linux x86-64

This is a deliberately partial Phase 2 implementation, not a Phase 2 completion
claim. On Linux x86-64, `milena build` emits an ELF64 `ET_EXEC` file directly:
the executable image, x86-64 instructions, and program header are written by
Milena itself. The path does not invoke a C compiler, assembler, linker, VM, or
interpreter. `MILENA_CC` is ignored.

The supported module/IR slice is `principal()` with no parameters and F64
return, plus one or more directly called helper functions with F64 parameters
and F64 return. Each accepted function may contain a verified acyclic
control-flow graph made of `MILENA_IR_BRANCH` and `MILENA_IR_COND_BRANCH` edges,
F64 constants,
add/subtract/multiply/divide, F64 comparisons producing BOOL, direct calls, and
returns. Conditional branches test canonical BOOL values; edge arguments are
copied through temporary stack slots before block parameters are written, so
parallel copies cannot overwrite one another. Native `JMP rel32` and `JNZ rel32`
targets are resolved against the final in-image basic-block offsets. The CFG
cycle check covers every block, including unreachable blocks; loops and cycles
are rejected fail-closed to avoid native/reference fuel mismatches. The evaluator
proves the selected path in each statically known call context. Block parameters
and edge arguments may carry only F64 or BOOL values; function parameters and
returns remain F64, and at most eight F64 call arguments are supported. Other
IR types/opcodes, malformed edges, unsupported block shapes, and CFGs not
validated by the typed-IR verifier are rejected. All functions must be reachable
from `principal()`. Calls are
emitted as real x86-64 `CALL rel32` instructions with resolved in-image targets;
F64 parameters and return values use the Linux x86-64 SysV ABI XMM registers
(XMM0–XMM7), so at most eight F64 arguments are supported. Each function has a
native stack frame, stores incoming parameters, and returns its computed value
in XMM0. The emitted call sequence is not constant-folded away.

For successful results, SSE2 `addsd`, `subsd`, `mulsd`, or `divsd` instructions
run at program runtime. The verifier uses checked integer arithmetic only to
prove exactness/range for constants and each statically known call context; it
does not produce program output. Finite exact integer-valued results in
`[-2^53, 2^53]` are supported. Signed-zero literals and zero-producing
arithmetic remain rejected; positive zero literals/arguments remain supported.
The returned F64 is converted and formatted at runtime using native integer
instructions and Linux syscalls. A division-by-zero operation, including in a
called helper, produces the same deterministic native error text with exit
status 70 as the typed-bytecode reference VM. Other out-of-range, fractional,
non-finite, or unsafe results fail closed without replacing existing output.

Recursion, CFG cycles/loops, unsupported types or opcodes, more than eight
function parameters/call arguments, unreachable functions, non-F64 function
signatures, or unsupported targets fail closed with explicit diagnostics.
Output installation is atomic and source/output aliasing is rejected. Windows/PE, Android/Bionic,
other Linux architectures, runtime-dependent calculations, and all language
constructs outside this narrow slice remain unsupported. This is not whole-
language AOT or full VM equivalence: Phase 1 remains incomplete, and the
normative Phase 2 acceptance gate still requires every Phase 1-supported
construct, resource/error equivalence, and validation on all agreed targets
including Android/Termux.

`make test-native-aot` verifies ELF identity, confirms runtime arithmetic and
direct call opcodes occur in executable text, executes a multi-argument helper
call and compares output against verified-bytecode VM execution, compares a
called-helper division-by-zero error against that VM, verifies fail-closed
behavior for unsupported control flow and out-of-slice values, and uses a
sentinel `MILENA_CC` executable to prove that AOT does not invoke an external
compiler.
