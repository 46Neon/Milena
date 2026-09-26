# Native AOT (Phase 1)

`milena build SOURCE -o OUTPUT` compiles the source through the official
lexer/parser/semantic frontend and `milena_canonical_program_compile_scalar_ir()`.
The backend consumes only the resulting verified `MilenaIRProgram` or
`MilenaIRModule`; it does not re-lower HIR and does not package or interpret
bytecode. This is a generated-C bootstrap backend, not a direct machine-code
code generator. It emits C for the selected typed-IR slice, then runs one host C
compiler executable with an argument vector (`fork`/`exec`, never a shell).
`MILENA_CC` may name that executable; the default is `cc`.

## Supported typed-IR slice

- One or more module functions with stable, unique symbol IDs, and exactly one
  zero-argument `principal()` entry point.
- Every function returns `F64`; every function parameter is `F64`.
- `F64` and boolean constants; `F64` addition, subtraction, multiplication,
  and division; `F64` equality/inequality and ordered comparisons.
- Direct calls among functions registered in the verified module, with exact
  `F64` signatures. The typed module verifier's rejection of direct/mutual
  recursion is required and preserved.
- CFG blocks, unconditional and boolean conditional branches, block parameters,
  and edge arguments. Edge argument copies are emitted as parallel copies so
  that cyclic assignments cannot overwrite a value before it is read.
- Numeric `F64` returns. Generated arithmetic rejects division by either
  positive or negative zero and terminates on a non-finite arithmetic or return
  value. Generated calls retain a 1,000-frame depth guard; the canonical module
  verifier also rejects recursive call graphs.

All other IR types and opcodes fail closed. In particular, this backend does
not compile integer arithmetic, boolean-returning functions, void functions,
indirect/external calls, recursive calls, data/array operations, or bytecode.
The source frontend and canonical typed-IR lowering impose their own narrower
limits; passing this list does not make unsupported source constructs valid.

## Host and output contract

Native compilation is currently POSIX-only (including Termux): it requires
`fork`, `execvp`, `waitpid`, `mkstemp`, and same-filesystem atomic `rename`.
The host must provide a C17 compiler and the standard math/runtime libraries.
Windows builds retain the CLI but report that native AOT is unavailable there.
The builder creates uniquely named source and candidate-binary files beside the
requested output, and replaces the destination only after successful native
compilation. A compiler or validation failure leaves an existing output intact.
Source/output paths that identify the same file (including hard links and
symlinks) are rejected. Generated function and value identifiers are mangled
from typed-IR symbol/value IDs; source identifiers are never copied into C.
