# Portable bytecode v1 — experimental slice

This document specifies the small, versioned bytecode/verifier/VM slice in
`include/bytecode.h` and `src/bytecode.c`. The bytecode is a portable serialized
instruction format; it is **not native machine code**. This slice is not yet
lowered from Milena source or from the canonical scalar/data HIR and is not
called by the shipped CLI/runtime. It is deliberately not included in the
production `SOURCES` list.

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

## Scope and gates still open

This is an isolated, tested foundation rather than compiler integration. It has
no function-call ABI, no lowering from `MilenaScalarHIR`/`MilenaDataHIR`, no
shared physical IR with the existing AOT path, and no strings/datasets/typed
data runtime. It does not replace the experimental legacy `src/vm.c`, whose
ownership and error-handling issues remain. It does not establish parity for
any `.milena` construction, nor does it mark compiler-plan phase 2 complete.

Run the focused regression suite with `make test-bytecode`; the suite is also a
dependency of `make test` and is wired to GCC, Clang, ASan, and UBSan CI jobs.
