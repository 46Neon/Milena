# Direct native AOT slice: Linux x86-64

This is a deliberately partial Phase 2 implementation, not a Phase 2 completion
claim. On Linux x86-64, `milena build` now emits an ELF64 `ET_EXEC` file directly:
the executable image, x86-64 instructions, and program header are written by
Milena itself. The path does not invoke a C compiler, assembler, linker, VM, or
interpreter. `MILENA_CC` is ignored.

The currently supported source/IR slice is a program with exactly one
parameterless `principal()` returning F64, represented by one verified basic
block and a constant expression consisting of F64 constants and add/subtract/
multiply/divide instructions followed by return. This implementation folds
that verified expression during compilation and emits a freestanding Linux
syscall program that writes the same `%.17g` result as the current CLI runtime.
Division by zero and non-finite results become native executables that report
the corresponding runtime error and exit with status 70. Other opcodes,
functions, parameters, and control-flow shapes are rejected with an explicit
diagnostic; output installation is atomic and source/output aliasing is
rejected.

The x86-64 ELF generator is intentionally host/target-specific. Windows/PE,
Android/Bionic, other Linux architectures, runtime-dependent calculations,
branches, calls, and all other language constructs remain unsupported. The
constant-folded result-string subset is only an initial native-code-generation
slice and is not a substitute for general instruction selection, runtime
support, or full VM equivalence. Phase 1 remains incomplete, and the normative
Phase 2 acceptance gate still requires every Phase 1-supported construct,
resource/error equivalence, and validation on all agreed targets including
Android/Termux.

`make test-native-aot` checks the emitted file's ELF identity and executable
behavior, compares the supported arithmetic result against the verified-bytecode
reference VM, verifies fail-closed behavior for unsupported control flow, and
uses a sentinel `MILENA_CC` executable to prove that AOT does not invoke an
external compiler.
