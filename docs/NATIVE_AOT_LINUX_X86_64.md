# Direct native AOT slice: Linux x86-64

This is a deliberately partial Phase 2 implementation, not a Phase 2 completion
claim. On Linux x86-64, `milena build` emits an ELF64 `ET_EXEC` file directly:
the executable image, x86-64 instructions, and program header are written by
Milena itself. The path does not invoke a C compiler, assembler, linker, VM, or
interpreter. `MILENA_CC` is ignored.

The supported source/IR slice remains exactly one parameterless
`principal()` returning F64, represented by one verified basic block containing
F64 constants and add/subtract/multiply/divide instructions followed by return.
For successful numeric results, the backend now emits and executes x86-64 SSE2
`addsd`, `subsd`, `mulsd`, or `divsd` instructions at program runtime. Constants
are loaded from the emitted instruction stream into stack slots, intermediate
values are computed by those native instructions, and the returned F64 is
converted and formatted at runtime using native integer instructions and Linux
syscalls. Finite, exact integer-valued results in the range `[-2^53, 2^53]` are
supported (arithmetic results of zero and signed-zero literals are rejected to
avoid formatting/IEEE-754 edge-case mismatches). The verifier uses checked
integer arithmetic only to establish that each accepted operation is exact and
within this subset; it does not produce the program's output. Division by zero
is retained as a deterministic native runtime error with exit status 70. Other
out-of-range, fractional, non-finite, unsupported, or unsafe values fail closed
at build time without replacing an existing output. The CLI output for accepted
values matches the typed-bytecode reference VM's `%.17g` output.

Other opcodes, functions, parameters, and control-flow shapes are rejected with
an explicit diagnostic; output installation is atomic and source/output
aliasing is rejected. The x86-64 ELF generator is intentionally host/target-
specific. Windows/PE, Android/Bionic, other Linux architectures,
runtime-dependent calculations, branches, calls, and all other language
constructs remain unsupported. This constrained instruction-selection slice is
not a substitute for general native code generation, runtime support, or full VM
equivalence. Phase 1 remains incomplete, and the normative Phase 2 acceptance
gate still requires every Phase 1-supported construct, resource/error
equivalence, and validation on all agreed targets including Android/Termux.

`make test-native-aot` verifies ELF identity, checks that each arithmetic opcode
is present in the executable's text bytes and compares executed results against
the verified-bytecode reference VM for addition, subtraction, multiplication,
and division. It also compares the division-by-zero error case, verifies
fail-closed behavior for unsupported control flow and out-of-slice numeric
values, and uses a sentinel `MILENA_CC` executable to prove that AOT does not
invoke an external compiler.
