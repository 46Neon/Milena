#include "bytecode.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BC_MAX_BYTES (MILENA_BYTECODE_HEADER_SIZE + \
                      (size_t)MILENA_BYTECODE_MAX_INSTRUCTIONS * \
                          MILENA_BYTECODE_INSTRUCTION_SIZE + \
                      MILENA_BYTECODE_MAX_REGISTERS + \
                      MILENA_BYTECODE_MAX_FUNCTIONS)

static const uint8_t bc_magic[4] = {'M', 'L', 'B', 'C'};

typedef struct {
    uint32_t max_instructions;
    uint16_t max_registers;
    size_t max_bytecode_bytes;
    uint64_t max_steps;
    uint16_t max_call_depth;
} ResolvedLimits;

static MilenaBytecodeStatus fail(MilenaBytecodeDiagnostic *diagnostic,
                                 MilenaBytecodeStatus status,
                                 size_t offset,
                                 const char *message) {
    if (diagnostic) {
        diagnostic->status = status;
        diagnostic->byte_offset = offset;
        (void)snprintf(diagnostic->message, sizeof(diagnostic->message), "%s",
                       message);
    }
    return status;
}

static void clear_diagnostic(MilenaBytecodeDiagnostic *diagnostic) {
    if (diagnostic) {
        diagnostic->status = MILENA_BC_OK;
        diagnostic->byte_offset = 0;
        diagnostic->message[0] = '\0';
    }
}

static MilenaBytecodeStatus resolve_limits(const MilenaBytecodeLimits *limits,
                                           ResolvedLimits *resolved,
                                           MilenaBytecodeDiagnostic *diagnostic) {
    if (!resolved) {
        return fail(diagnostic, MILENA_BC_ARGUMENT, 0, "missing limits output");
    }
    resolved->max_instructions = MILENA_BYTECODE_MAX_INSTRUCTIONS;
    resolved->max_registers = MILENA_BYTECODE_MAX_REGISTERS;
    resolved->max_bytecode_bytes = BC_MAX_BYTES;
    resolved->max_steps = MILENA_BYTECODE_DEFAULT_STEP_LIMIT;
    resolved->max_call_depth = MILENA_BYTECODE_MAX_CALL_DEPTH;
    if (limits) {
        if (limits->max_instructions > MILENA_BYTECODE_MAX_INSTRUCTIONS ||
            limits->max_registers > MILENA_BYTECODE_MAX_REGISTERS ||
            limits->max_bytecode_bytes > BC_MAX_BYTES ||
            limits->max_steps > MILENA_BYTECODE_MAX_STEP_LIMIT ||
            limits->max_call_depth > MILENA_BYTECODE_MAX_CALL_DEPTH) {
            return fail(diagnostic, MILENA_BC_LIMIT_EXCEEDED, 0,
                        "configured limit exceeds the hard safety cap");
        }
        if (limits->max_instructions != 0) {
            resolved->max_instructions = limits->max_instructions;
        }
        if (limits->max_registers != 0) {
            resolved->max_registers = limits->max_registers;
        }
        if (limits->max_bytecode_bytes != 0) {
            resolved->max_bytecode_bytes = limits->max_bytecode_bytes;
        }
        if (limits->max_steps != 0) {
            resolved->max_steps = limits->max_steps;
        }
        if (limits->max_call_depth != 0) {
            resolved->max_call_depth = limits->max_call_depth;
        }
    }
    return MILENA_BC_OK;
}

static uint16_t read_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64le(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static void write_u16le(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & UINT16_C(0xff));
    p[1] = (uint8_t)((value >> 8) & UINT16_C(0xff));
}

static void write_u32le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & UINT32_C(0xff));
    p[1] = (uint8_t)((value >> 8) & UINT32_C(0xff));
    p[2] = (uint8_t)((value >> 16) & UINT32_C(0xff));
    p[3] = (uint8_t)((value >> 24) & UINT32_C(0xff));
}

static void write_u64le(uint8_t *p, uint64_t value) {
    p[0] = (uint8_t)(value & UINT64_C(0xff));
    p[1] = (uint8_t)((value >> 8) & UINT64_C(0xff));
    p[2] = (uint8_t)((value >> 16) & UINT64_C(0xff));
    p[3] = (uint8_t)((value >> 24) & UINT64_C(0xff));
    p[4] = (uint8_t)((value >> 32) & UINT64_C(0xff));
    p[5] = (uint8_t)((value >> 40) & UINT64_C(0xff));
    p[6] = (uint8_t)((value >> 48) & UINT64_C(0xff));
    p[7] = (uint8_t)((value >> 56) & UINT64_C(0xff));
}

static bool host_has_binary64(void) {
    return sizeof(double) == sizeof(uint64_t) && FLT_RADIX == 2 &&
           DBL_MANT_DIG == 53 && DBL_MIN_EXP == -1021 && DBL_MAX_EXP == 1024;
}

static double bits_to_double(uint64_t bits) {
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t double_to_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool milena_bytecode_encoded_size(size_t instruction_count, size_t *size_out) {
    size_t body_size;
    if (!size_out || instruction_count == 0 ||
        instruction_count > MILENA_BYTECODE_MAX_INSTRUCTIONS ||
        instruction_count > (SIZE_MAX - MILENA_BYTECODE_HEADER_SIZE) /
                                MILENA_BYTECODE_INSTRUCTION_SIZE) {
        return false;
    }
    body_size = instruction_count * MILENA_BYTECODE_INSTRUCTION_SIZE;
    *size_out = MILENA_BYTECODE_HEADER_SIZE + body_size;
    return true;
}

bool milena_bytecode_typed_encoded_size(size_t instruction_count,
                                        uint16_t register_count,
                                        uint16_t function_count,
                                        size_t *size_out) {
    size_t base_size;
    if (!size_out || register_count == 0 ||
        register_count > MILENA_BYTECODE_MAX_REGISTERS ||
        function_count == 0 || function_count > MILENA_BYTECODE_MAX_FUNCTIONS ||
        !milena_bytecode_encoded_size(instruction_count, &base_size) ||
        (size_t)register_count + function_count > SIZE_MAX - base_size) return false;
    *size_out = base_size + register_count + function_count;
    return true;
}

static MilenaBytecodeStatus verify_internal(const uint8_t *bytes,
                                            size_t length,
                                            const ResolvedLimits *limits,
                                            MilenaBytecodeDiagnostic *diagnostic,
                                            uint16_t *register_count_out,
                                            uint32_t *instruction_count_out) {
    uint16_t register_count;
    uint16_t minor_version;
    uint32_t instruction_count;
    struct FunctionInfo { uint32_t start, end, parameter_base, arity; uint8_t return_type; } functions[MILENA_BYTECODE_MAX_FUNCTIONS];
    const uint8_t *register_types = NULL;
    const uint8_t *function_return_types = NULL;
    uint8_t call_edges[MILENA_BYTECODE_MAX_FUNCTIONS][MILENA_BYTECODE_MAX_FUNCTIONS] = {{0}};
    uint32_t function_count = 0;
    uint32_t *owners = NULL;
    size_t expected_size;
    size_t body_size;
    uint8_t *visited = NULL;
    uint32_t *queue = NULL;
    size_t head = 0;
    size_t tail = 0;
    bool reachable_return = false;

    if (!bytes) {
        return fail(diagnostic, MILENA_BC_ARGUMENT, 0, "bytecode is null");
    }
    if (length < MILENA_BYTECODE_HEADER_SIZE) {
        return fail(diagnostic, MILENA_BC_BAD_FORMAT, length,
                    "truncated bytecode header");
    }
    if (length > limits->max_bytecode_bytes) {
        return fail(diagnostic, MILENA_BC_LIMIT_EXCEEDED, 0,
                    "bytecode exceeds configured size limit");
    }
    if (memcmp(bytes, bc_magic, sizeof(bc_magic)) != 0) {
        return fail(diagnostic, MILENA_BC_BAD_MAGIC, 0, "invalid bytecode magic");
    }
    if (read_u16le(bytes + 4) != MILENA_BYTECODE_VERSION_MAJOR ||
        read_u16le(bytes + 6) > MILENA_BYTECODE_MAX_MINOR) {
        return fail(diagnostic, MILENA_BC_BAD_VERSION, 4,
                    "unsupported bytecode version");
    }
    minor_version = read_u16le(bytes + 6);
    register_count = read_u16le(bytes + 8);
    if (read_u16le(bytes + 10) != 0) {
        return fail(diagnostic, MILENA_BC_BAD_FORMAT, 10,
                    "reserved header bits must be zero");
    }
    instruction_count = read_u32le(bytes + 12);
    if (register_count == 0 || register_count > limits->max_registers ||
        register_count > MILENA_BYTECODE_MAX_REGISTERS ||
        instruction_count == 0 ||
        instruction_count > limits->max_instructions ||
        instruction_count > MILENA_BYTECODE_MAX_INSTRUCTIONS) {
        return fail(diagnostic, MILENA_BC_LIMIT_EXCEEDED, 8,
                    "register or instruction count is outside configured limits");
    }
    body_size = (size_t)instruction_count * MILENA_BYTECODE_INSTRUCTION_SIZE;
    if (body_size > SIZE_MAX - MILENA_BYTECODE_HEADER_SIZE) {
        return fail(diagnostic, MILENA_BC_BAD_FORMAT, 12,
                    "bytecode length overflows host size");
    }
    expected_size = MILENA_BYTECODE_HEADER_SIZE + body_size;
    if (minor_version < MILENA_BYTECODE_VERSION_TYPED_MINOR) {
        if (length != expected_size) {
            return fail(diagnostic, MILENA_BC_BAD_FORMAT,
                        length < expected_size ? length : expected_size,
                        "bytecode length does not match instruction count");
        }
    } else if (length < expected_size + register_count) {
        return fail(diagnostic, MILENA_BC_BAD_FORMAT, length,
                    "truncated v1.2 register type table");
    }
    if (!host_has_binary64()) {
        return fail(diagnostic, MILENA_BC_BAD_FORMAT, 0,
                    "host does not support IEEE-754 binary64 values");
    }

    if (minor_version >= MILENA_BYTECODE_VERSION_CALL_MINOR) {
        if (bytes[MILENA_BYTECODE_HEADER_SIZE] != MILENA_BC_FUNCTION) {
            return fail(diagnostic, MILENA_BC_BAD_FORMAT, MILENA_BYTECODE_HEADER_SIZE,
                        "v1.1 must begin with function zero");
        }
        for (uint32_t pc = 0; pc < instruction_count; ++pc) {
            const size_t offset = MILENA_BYTECODE_HEADER_SIZE +
                                  (size_t)pc * MILENA_BYTECODE_INSTRUCTION_SIZE;
            const uint8_t *record = bytes + offset;
            if (record[0] != MILENA_BC_FUNCTION) continue;
            uint32_t id = read_u32le(record + 4);
            uint32_t parameter_base = read_u32le(record + 8);
            uint32_t arity = read_u32le(record + 12);
            if (function_count >= MILENA_BYTECODE_MAX_FUNCTIONS ||
                id != function_count || (function_count == 0 && pc != 0) ||
                parameter_base > register_count ||
                arity > register_count - parameter_base || arity > MILENA_BYTECODE_MAX_REGISTERS) {
                return fail(diagnostic, MILENA_BC_BAD_OPERAND, offset,
                            "invalid function id, parameter range, or function limit");
            }
            if (function_count) functions[function_count - 1u].end = pc;
            functions[function_count] = (struct FunctionInfo){
                pc + 1u, instruction_count, parameter_base, arity, 0
            };
            function_count++;
        }
        if (!function_count) {
            return fail(diagnostic, MILENA_BC_BAD_FORMAT, MILENA_BYTECODE_HEADER_SIZE,
                        "v1.1 program has no function table");
        }
        for (uint32_t f = 0; f < function_count; ++f) {
            if (functions[f].start >= functions[f].end) {
                return fail(diagnostic, MILENA_BC_BAD_CONTROL_FLOW,
                            MILENA_BYTECODE_HEADER_SIZE +
                                (size_t)(functions[f].start - 1u) * MILENA_BYTECODE_INSTRUCTION_SIZE,
                            "function body is empty");
            }
        }
        owners = (uint32_t *)malloc((size_t)instruction_count * sizeof(*owners));
        if (!owners) return fail(diagnostic, MILENA_BC_OUT_OF_MEMORY, 0,
                                 "unable to allocate function ownership map");
        for (uint32_t f = 0; f < function_count; ++f)
            for (uint32_t pc = functions[f].start - 1u; pc < functions[f].end; ++pc)
                owners[pc] = f;
    }

    if (minor_version == MILENA_BYTECODE_VERSION_TYPED_MINOR) {
        const size_t metadata_offset = expected_size;
        size_t typed_size = 0;
        if (!milena_bytecode_typed_encoded_size(instruction_count, register_count,
                                                (uint16_t)function_count,
                                                &typed_size) || length != typed_size) {
            free(owners);
            return fail(diagnostic, MILENA_BC_BAD_FORMAT,
                        length < typed_size ? length : typed_size,
                        "v1.2 type table length does not match register/function counts");
        }
        register_types = bytes + metadata_offset;
        function_return_types = register_types + register_count;
        if (!function_count || function_return_types[0] != MILENA_BC_TYPE_NUMBER) {
            free(owners);
            return fail(diagnostic, MILENA_BC_BAD_FORMAT, metadata_offset + register_count,
                        "v1.2 entry function must declare a numeric return");
        }
        for (uint32_t r = 0; r < register_count; ++r) {
            if (register_types[r] != MILENA_BC_TYPE_NUMBER &&
                register_types[r] != MILENA_BC_TYPE_BOOLEAN) {
                free(owners);
                return fail(diagnostic, MILENA_BC_BAD_FORMAT, metadata_offset + r,
                            "invalid v1.2 register type tag");
            }
        }
        for (uint32_t f = 0; f < function_count; ++f) {
            if (function_return_types[f] != MILENA_BC_TYPE_NUMBER &&
                function_return_types[f] != MILENA_BC_TYPE_BOOLEAN) {
                free(owners);
                return fail(diagnostic, MILENA_BC_BAD_FORMAT,
                            metadata_offset + register_count + f,
                            "invalid v1.2 function return type tag");
            }
            functions[f].return_type = function_return_types[f];
        }
    }

    for (uint32_t pc = 0; pc < instruction_count; ++pc) {
        const size_t offset = MILENA_BYTECODE_HEADER_SIZE +
                              (size_t)pc * MILENA_BYTECODE_INSTRUCTION_SIZE;
        const uint8_t *record = bytes + offset;
        const uint8_t opcode = record[0];
        const uint32_t a = read_u32le(record + 4);
        const uint32_t b = read_u32le(record + 8);
        const uint32_t c = read_u32le(record + 12);
        const uint64_t immediate = read_u64le(record + 16);
        bool registers_valid = true;

        if (record[1] != 0 || record[2] != 0 || record[3] != 0) {
            free(owners);
            return fail(diagnostic, MILENA_BC_BAD_FORMAT, offset + 1,
                        "instruction reserved bits must be zero");
        }
        switch (opcode) {
            case MILENA_BC_CONST_F64:
                registers_valid = a < register_count && b == 0 && c == 0 &&
                                  isfinite(bits_to_double(immediate));
                break;
            case MILENA_BC_CONST_BOOL: {
                double boolean_value = bits_to_double(immediate);
                registers_valid = minor_version == MILENA_BYTECODE_VERSION_TYPED_MINOR &&
                                  a < register_count && b == 0 && c == 0 &&
                                  (boolean_value == 0.0 || boolean_value == 1.0);
                break;
            }
            case MILENA_BC_MOVE:
            case MILENA_BC_NEG:
                registers_valid = a < register_count && b < register_count &&
                                  c == 0 && immediate == 0;
                break;
            case MILENA_BC_ADD:
            case MILENA_BC_SUB:
            case MILENA_BC_MUL:
            case MILENA_BC_DIV:
            case MILENA_BC_EQ:
            case MILENA_BC_NE:
            case MILENA_BC_LT:
            case MILENA_BC_LE:
            case MILENA_BC_GT:
            case MILENA_BC_GE:
                registers_valid = a < register_count && b < register_count &&
                                  c < register_count && immediate == 0;
                break;
            case MILENA_BC_JUMP:
                registers_valid = a < instruction_count && b == 0 && c == 0 &&
                                  immediate == 0;
                break;
            case MILENA_BC_JUMP_IF_FALSE:
                registers_valid = a < register_count && b < instruction_count &&
                                  c == 0 && immediate == 0;
                break;
            case MILENA_BC_RETURN:
                registers_valid = a < register_count && b == 0 && c == 0 &&
                                  immediate == 0;
                break;
            case MILENA_BC_FUNCTION:
                registers_valid = minor_version >= MILENA_BYTECODE_VERSION_CALL_MINOR && a < function_count &&
                                  a < MILENA_BYTECODE_MAX_FUNCTIONS &&
                                  b <= register_count && c <= register_count - b &&
                                  immediate == 0 &&
                                  (a < function_count && functions[a].start == pc + 1u &&
                                   functions[a].parameter_base == b && functions[a].arity == c);
                break;
            case MILENA_BC_CALL: {
                double encoded_arity = bits_to_double(immediate);
                registers_valid = minor_version >= MILENA_BYTECODE_VERSION_CALL_MINOR && a < register_count &&
                                  b < function_count && isfinite(encoded_arity) &&
                                  encoded_arity >= 0.0 && encoded_arity <= MILENA_BYTECODE_MAX_REGISTERS &&
                                  floor(encoded_arity) == encoded_arity &&
                                  c <= register_count && (uint32_t)encoded_arity <= register_count - c &&
                                  (uint32_t)encoded_arity == functions[b].arity;
                if (registers_valid) call_edges[owners[pc]][b] = 1;
                break;
            }
            default:
                free(owners);
                return fail(diagnostic, MILENA_BC_BAD_OPCODE, offset,
                            "unknown bytecode opcode");
        }
        if (registers_valid && minor_version >= MILENA_BYTECODE_VERSION_CALL_MINOR &&
            (opcode == MILENA_BC_JUMP || opcode == MILENA_BC_JUMP_IF_FALSE)) {
            uint32_t target = opcode == MILENA_BC_JUMP ? a : b;
            if (target >= instruction_count || owners[target] != owners[pc] ||
                bytes[MILENA_BYTECODE_HEADER_SIZE +
                      (size_t)target * MILENA_BYTECODE_INSTRUCTION_SIZE] == MILENA_BC_FUNCTION)
                registers_valid = false;
        }
        if (!registers_valid) {
            free(owners);
            return fail(diagnostic, MILENA_BC_BAD_OPERAND, offset,
                        "invalid register, function, target, or immediate operand");
        }
        if (minor_version == MILENA_BYTECODE_VERSION_TYPED_MINOR) {
            bool types_valid = true;
            const uint8_t number = MILENA_BC_TYPE_NUMBER;
            const uint8_t boolean = MILENA_BC_TYPE_BOOLEAN;
            switch (opcode) {
                case MILENA_BC_CONST_F64:
                    types_valid = register_types[a] == number;
                    break;
                case MILENA_BC_CONST_BOOL:
                    types_valid = register_types[a] == boolean;
                    break;
                case MILENA_BC_MOVE:
                    types_valid = register_types[a] == register_types[b];
                    break;
                case MILENA_BC_NEG:
                    types_valid = register_types[a] == number && register_types[b] == number;
                    break;
                case MILENA_BC_ADD: case MILENA_BC_SUB: case MILENA_BC_MUL: case MILENA_BC_DIV:
                    types_valid = register_types[a] == number && register_types[b] == number &&
                                  register_types[c] == number;
                    break;
                case MILENA_BC_EQ: case MILENA_BC_NE:
                    types_valid = register_types[b] == register_types[c] &&
                                  register_types[a] == boolean;
                    break;
                case MILENA_BC_LT: case MILENA_BC_LE: case MILENA_BC_GT: case MILENA_BC_GE:
                    types_valid = register_types[b] == number && register_types[c] == number &&
                                  register_types[a] == boolean;
                    break;
                case MILENA_BC_JUMP_IF_FALSE:
                    types_valid = register_types[a] == boolean;
                    break;
                case MILENA_BC_RETURN:
                    types_valid = register_types[a] == functions[owners[pc]].return_type;
                    break;
                case MILENA_BC_CALL: {
                    uint32_t arity = (uint32_t)bits_to_double(immediate);
                    types_valid = register_types[a] == functions[b].return_type;
                    for (uint32_t i = 0; types_valid && i < arity; ++i)
                        types_valid = register_types[c + i] ==
                                      register_types[functions[b].parameter_base + i];
                    break;
                }
                case MILENA_BC_JUMP: case MILENA_BC_FUNCTION:
                    break;
                default:
                    types_valid = false;
                    break;
            }
            if (!types_valid) {
                free(owners);
                return fail(diagnostic, MILENA_BC_BAD_TYPE, offset,
                            "v1.2 opcode operands disagree with declared types");
            }
        }
    }

    if (minor_version >= MILENA_BYTECODE_VERSION_CALL_MINOR) {
        /* Calls must form a DAG: this increment deliberately rejects recursion. */
        uint8_t color[MILENA_BYTECODE_MAX_FUNCTIONS] = {0};
        for (uint32_t root = 0; root < function_count; ++root) {
            uint32_t stack[MILENA_BYTECODE_MAX_FUNCTIONS];
            uint32_t next[MILENA_BYTECODE_MAX_FUNCTIONS] = {0};
            size_t depth = 0;
            if (color[root]) continue;
            stack[depth++] = root;
            while (depth) {
                uint32_t f = stack[depth - 1u];
                if (!color[f]) color[f] = 1;
                while (next[depth - 1u] < function_count &&
                       !call_edges[f][next[depth - 1u]]) next[depth - 1u]++;
                if (next[depth - 1u] == function_count) {
                    color[f] = 2; depth--; continue;
                }
                uint32_t target = next[depth - 1u]++;
                if (color[target] == 1) {
                    free(owners);
                    return fail(diagnostic, MILENA_BC_BAD_CONTROL_FLOW,
                                MILENA_BYTECODE_HEADER_SIZE,
                                "recursive call graph is not supported in v1.1");
                }
                if (!color[target]) {
                    if (depth >= MILENA_BYTECODE_MAX_CALL_DEPTH) {
                        free(owners);
                        return fail(diagnostic, MILENA_BC_LIMIT_EXCEEDED, 0,
                                    "call graph exceeds the bounded frame depth");
                    }
                    stack[depth] = target;
                    next[depth] = 0;
                    depth++;
                }
            }
        }
        visited = (uint8_t *)calloc((size_t)instruction_count, sizeof(*visited));
        queue = (uint32_t *)malloc((size_t)instruction_count * sizeof(*queue));
        if (!visited || !queue) {
            free(visited); free(queue); free(owners);
            return fail(diagnostic, MILENA_BC_OUT_OF_MEMORY, 0,
                        "unable to allocate verifier control-flow workspace");
        }
        for (uint32_t f = 0; f < function_count; ++f) {
            memset(visited, 0, instruction_count);
            head = tail = 0;
            reachable_return = false;
            visited[functions[f].start] = 1;
            queue[tail++] = functions[f].start;
            while (head < tail) {
                const uint32_t pc = queue[head++];
                const size_t offset = MILENA_BYTECODE_HEADER_SIZE +
                                      (size_t)pc * MILENA_BYTECODE_INSTRUCTION_SIZE;
                const uint8_t *record = bytes + offset;
                uint8_t op = record[0];
                uint32_t a = read_u32le(record + 4), b = read_u32le(record + 8);
                uint32_t successors[2]; size_t n = 0;
                if (owners[pc] != f || op == MILENA_BC_FUNCTION) {
                    free(visited); free(queue); free(owners);
                    return fail(diagnostic, MILENA_BC_BAD_CONTROL_FLOW, offset,
                                "control flow enters another function");
                }
                if (op == MILENA_BC_RETURN) { reachable_return = true; continue; }
                if (op == MILENA_BC_JUMP) successors[n++] = a;
                else if (op == MILENA_BC_JUMP_IF_FALSE) {
                    successors[n++] = b;
                    if (pc + 1u >= functions[f].end) {
                        free(visited); free(queue); free(owners);
                        return fail(diagnostic, MILENA_BC_BAD_CONTROL_FLOW, offset,
                                    "conditional branch falls through function end");
                    }
                    successors[n++] = pc + 1u;
                } else {
                    if (pc + 1u >= functions[f].end) {
                        free(visited); free(queue); free(owners);
                        return fail(diagnostic, MILENA_BC_BAD_CONTROL_FLOW, offset,
                                    "reachable instruction falls through function end");
                    }
                    successors[n++] = pc + 1u;
                }
                for (size_t i = 0; i < n; ++i) {
                    uint32_t to = successors[i];
                    if (to >= instruction_count || owners[to] != f) {
                        free(visited); free(queue); free(owners);
                        return fail(diagnostic, MILENA_BC_BAD_CONTROL_FLOW, offset,
                                    "branch target is outside its function");
                    }
                    if (!visited[to]) { visited[to] = 1; queue[tail++] = to; }
                }
            }
            if (!reachable_return) {
                free(visited); free(queue); free(owners);
                return fail(diagnostic, MILENA_BC_BAD_CONTROL_FLOW,
                            MILENA_BYTECODE_HEADER_SIZE +
                                (size_t)functions[f].start * MILENA_BYTECODE_INSTRUCTION_SIZE,
                            "function has no reachable return");
            }
        }
        free(visited); free(queue); free(owners);
        if (register_count_out) *register_count_out = register_count;
        if (instruction_count_out) *instruction_count_out = instruction_count;
        clear_diagnostic(diagnostic);
        return MILENA_BC_OK;
    }

    visited = (uint8_t *)calloc((size_t)instruction_count, sizeof(*visited));
    queue = (uint32_t *)malloc((size_t)instruction_count * sizeof(*queue));
    if (!visited || !queue) {
        free(visited);
        free(queue);
        return fail(diagnostic, MILENA_BC_OUT_OF_MEMORY, 0,
                    "unable to allocate verifier control-flow workspace");
    }
    visited[0] = 1;
    queue[tail++] = 0;
    while (head < tail) {
        const uint32_t pc = queue[head++];
        const size_t offset = MILENA_BYTECODE_HEADER_SIZE +
                              (size_t)pc * MILENA_BYTECODE_INSTRUCTION_SIZE;
        const uint8_t *record = bytes + offset;
        const uint8_t opcode = record[0];
        const uint32_t a = read_u32le(record + 4);
        const uint32_t b = read_u32le(record + 8);
        uint32_t successors[2];
        size_t successor_count = 0;

        if (opcode == MILENA_BC_RETURN) {
            reachable_return = true;
            continue;
        }
        if (opcode == MILENA_BC_JUMP) {
            successors[successor_count++] = a;
        } else if (opcode == MILENA_BC_JUMP_IF_FALSE) {
            successors[successor_count++] = b;
            if (pc == instruction_count - 1) {
                free(visited);
                free(queue);
                return fail(diagnostic, MILENA_BC_BAD_CONTROL_FLOW, offset,
                            "conditional branch falls off the instruction stream");
            }
            successors[successor_count++] = pc + 1;
        } else {
            if (pc == instruction_count - 1) {
                free(visited);
                free(queue);
                return fail(diagnostic, MILENA_BC_BAD_CONTROL_FLOW, offset,
                            "reachable instruction falls off without returning");
            }
            successors[successor_count++] = pc + 1;
        }
        for (size_t i = 0; i < successor_count; ++i) {
            const uint32_t successor = successors[i];
            if (visited[successor] == 0) {
                visited[successor] = 1;
                queue[tail++] = successor;
            }
        }
    }
    free(visited);
    free(queue);
    if (!reachable_return) {
        return fail(diagnostic, MILENA_BC_BAD_CONTROL_FLOW, 0,
                    "no return instruction is reachable from entry");
    }
    if (register_count_out) {
        *register_count_out = register_count;
    }
    if (instruction_count_out) {
        *instruction_count_out = instruction_count;
    }
    clear_diagnostic(diagnostic);
    return MILENA_BC_OK;
}

MilenaBytecodeStatus milena_bytecode_verify(
    const uint8_t *bytes,
    size_t length,
    const MilenaBytecodeLimits *limits,
    MilenaBytecodeDiagnostic *diagnostic) {
    ResolvedLimits resolved;
    MilenaBytecodeStatus status;
    clear_diagnostic(diagnostic);
    status = resolve_limits(limits, &resolved, diagnostic);
    if (status != MILENA_BC_OK) {
        return status;
    }
    return verify_internal(bytes, length, &resolved, diagnostic, NULL, NULL);
}

MilenaBytecodeStatus milena_bytecode_encode(
    const MilenaBytecodeProgram *program,
    uint8_t *out,
    size_t capacity,
    size_t *written,
    const MilenaBytecodeLimits *limits,
    MilenaBytecodeDiagnostic *diagnostic) {
    ResolvedLimits resolved;
    size_t required;
    size_t base_required;
    uint16_t encoded_function_count = 0;
    uint8_t *temporary;
    MilenaBytecodeStatus status;

    clear_diagnostic(diagnostic);
    if (!written) {
        return fail(diagnostic, MILENA_BC_ARGUMENT, 0,
                    "encoded-size output is required");
    }
    *written = 0;
    status = resolve_limits(limits, &resolved, diagnostic);
    if (status != MILENA_BC_OK) {
        return status;
    }
    if (!program) {
        return fail(diagnostic, MILENA_BC_ARGUMENT, 0, "program is null");
    }
    if (program->major != MILENA_BYTECODE_VERSION_MAJOR ||
        program->minor > MILENA_BYTECODE_MAX_MINOR) {
        return fail(diagnostic, MILENA_BC_BAD_VERSION, 4,
                    "encoder only supports bytecode v1.0 through v1.2");
    }
    if (program->register_count == 0 ||
        program->register_count > resolved.max_registers ||
        program->instruction_count == 0 ||
        program->instruction_count > resolved.max_instructions ||
        !program->instructions) {
        return fail(diagnostic, MILENA_BC_LIMIT_EXCEEDED, 8,
                    "program exceeds configured registers/instructions or is empty");
    }
    if (!milena_bytecode_encoded_size(program->instruction_count, &base_required)) {
        return fail(diagnostic, MILENA_BC_LIMIT_EXCEEDED, 12,
                    "encoded program exceeds configured size limit");
    }
    required = base_required;
    if (program->minor == MILENA_BYTECODE_VERSION_TYPED_MINOR) {
        for (size_t i = 0; i < program->instruction_count; ++i) {
            if (program->instructions[i].opcode == MILENA_BC_FUNCTION) {
                if (encoded_function_count >= MILENA_BYTECODE_MAX_FUNCTIONS) {
                    return fail(diagnostic, MILENA_BC_BAD_FORMAT,
                                MILENA_BYTECODE_HEADER_SIZE + i * MILENA_BYTECODE_INSTRUCTION_SIZE,
                                "v1.2 exceeds the function type table limit");
                }
                encoded_function_count++;
            }
        }
        if (program->register_type_count != program->register_count ||
            program->function_return_type_count != encoded_function_count ||
            !program->register_types ||
            (encoded_function_count && !program->function_return_types) ||
            !milena_bytecode_typed_encoded_size(program->instruction_count,
                program->register_count, encoded_function_count, &required)) {
            return fail(diagnostic, MILENA_BC_BAD_FORMAT, 12,
                        "v1.2 requires exact register and function type tables");
        }
    } else if (program->register_types || program->register_type_count ||
               program->function_return_types || program->function_return_type_count) {
        return fail(diagnostic, MILENA_BC_BAD_FORMAT, 12,
                    "type metadata is only valid in bytecode v1.2");
    }
    if (required > resolved.max_bytecode_bytes) {
        return fail(diagnostic, MILENA_BC_LIMIT_EXCEEDED, 12,
                    "encoded program exceeds configured size limit");
    }
    *written = required;
    if (!out && capacity == 0) {
        return fail(diagnostic, MILENA_BC_BUFFER_TOO_SMALL, required,
                    "output buffer size query");
    }
    if (!out) {
        *written = 0;
        return fail(diagnostic, MILENA_BC_ARGUMENT, 0,
                    "output is null but capacity is nonzero");
    }
    if (capacity < required) {
        return fail(diagnostic, MILENA_BC_BUFFER_TOO_SMALL, capacity,
                    "output buffer is too small");
    }
    temporary = (uint8_t *)calloc(required, sizeof(*temporary));
    if (!temporary) {
        *written = 0;
        return fail(diagnostic, MILENA_BC_OUT_OF_MEMORY, 0,
                    "unable to allocate encoding workspace");
    }

    memcpy(temporary, bc_magic, sizeof(bc_magic));
    write_u16le(temporary + 4, program->major);
    write_u16le(temporary + 6, program->minor);
    write_u16le(temporary + 8, program->register_count);
    write_u16le(temporary + 10, 0);
    write_u32le(temporary + 12, (uint32_t)program->instruction_count);
    for (size_t i = 0; i < program->instruction_count; ++i) {
        const MilenaBytecodeInstruction *source = &program->instructions[i];
        uint8_t *record = temporary + MILENA_BYTECODE_HEADER_SIZE +
                          i * MILENA_BYTECODE_INSTRUCTION_SIZE;
        if (source->opcode != MILENA_BC_CONST_F64 &&
            source->opcode != MILENA_BC_CONST_BOOL &&
            source->opcode != MILENA_BC_CALL && source->immediate != 0.0) {
            free(temporary);
            *written = 0;
            return fail(diagnostic, MILENA_BC_BAD_OPERAND,
                        MILENA_BYTECODE_HEADER_SIZE +
                            i * MILENA_BYTECODE_INSTRUCTION_SIZE,
                        "only constants and v1.1+ CALL may carry an immediate");
        }
        record[0] = source->opcode;
        write_u32le(record + 4, source->a);
        write_u32le(record + 8, source->b);
        write_u32le(record + 12, source->c);
        if (source->opcode == MILENA_BC_CONST_F64 ||
            source->opcode == MILENA_BC_CONST_BOOL ||
            source->opcode == MILENA_BC_CALL) {
            if (!host_has_binary64()) {
                free(temporary);
                *written = 0;
                return fail(diagnostic, MILENA_BC_BAD_FORMAT, 0,
                            "host does not support IEEE-754 binary64 values");
            }
            write_u64le(record + 16, double_to_bits(source->immediate));
        }
    }
    if (program->minor == MILENA_BYTECODE_VERSION_TYPED_MINOR) {
        memcpy(temporary + base_required, program->register_types,
               program->register_count);
        memcpy(temporary + base_required + program->register_count,
               program->function_return_types, encoded_function_count);
    }

    status = verify_internal(temporary, required, &resolved, diagnostic, NULL, NULL);
    if (status == MILENA_BC_OK) {
        memcpy(out, temporary, required);
    } else {
        *written = 0;
    }
    free(temporary);
    return status;
}

static bool read_instruction(const uint8_t *bytes,
                             uint32_t pc,
                             uint32_t *a,
                             uint32_t *b,
                             uint32_t *c,
                             double *immediate) {
    const size_t offset = MILENA_BYTECODE_HEADER_SIZE +
                          (size_t)pc * MILENA_BYTECODE_INSTRUCTION_SIZE;
    const uint8_t *record = bytes + offset;
    *a = read_u32le(record + 4);
    *b = read_u32le(record + 8);
    *c = read_u32le(record + 12);
    *immediate = bits_to_double(read_u64le(record + 16));
    return true;
}

MilenaBytecodeStatus milena_bytecode_run(
    const uint8_t *bytes,
    size_t length,
    const MilenaBytecodeLimits *limits,
    double *result,
    MilenaBytecodeDiagnostic *diagnostic) {
    ResolvedLimits resolved;
    uint16_t register_count;
    uint32_t instruction_count;
    double *registers = NULL;
    uint32_t pc = 0;
    uint64_t steps = 0;
    struct { uint32_t return_pc, destination; } frames[MILENA_BYTECODE_MAX_CALL_DEPTH];
    uint32_t function_starts[MILENA_BYTECODE_MAX_FUNCTIONS] = {0};
    uint32_t parameter_bases[MILENA_BYTECODE_MAX_FUNCTIONS] = {0};
    uint32_t arities[MILENA_BYTECODE_MAX_FUNCTIONS] = {0};
    uint32_t function_count = 0, frame_depth = 0;
    MilenaBytecodeStatus status;

    clear_diagnostic(diagnostic);
    if (!result) {
        return fail(diagnostic, MILENA_BC_ARGUMENT, 0,
                    "execution result output is required");
    }
    *result = 0.0;
    status = resolve_limits(limits, &resolved, diagnostic);
    if (status != MILENA_BC_OK) {
        return status;
    }
    status = verify_internal(bytes, length, &resolved, diagnostic,
                             &register_count, &instruction_count);
    if (status != MILENA_BC_OK) {
        return status;
    }
    registers = (double *)calloc((size_t)register_count, sizeof(*registers));
    if (!registers) {
        return fail(diagnostic, MILENA_BC_OUT_OF_MEMORY, 0,
                    "unable to allocate per-run registers");
    }
    if (read_u16le(bytes + 6) >= MILENA_BYTECODE_VERSION_CALL_MINOR) {
        for (uint32_t i = 0; i < instruction_count; ++i) {
            const size_t off = MILENA_BYTECODE_HEADER_SIZE +
                               (size_t)i * MILENA_BYTECODE_INSTRUCTION_SIZE;
            if (bytes[off] != MILENA_BC_FUNCTION) continue;
            uint32_t id = read_u32le(bytes + off + 4);
            if (id >= MILENA_BYTECODE_MAX_FUNCTIONS) {
                free(registers);
                return fail(diagnostic, MILENA_BC_BAD_OPERAND, off,
                            "function id exceeds runtime table");
            }
            function_starts[id] = i + 1u;
            parameter_bases[id] = read_u32le(bytes + off + 8);
            arities[id] = read_u32le(bytes + off + 12);
            if (function_count <= id) function_count = id + 1u;
        }
        if (!function_count) {
            free(registers);
            return fail(diagnostic, MILENA_BC_BAD_FORMAT, MILENA_BYTECODE_HEADER_SIZE,
                        "verified program has no entry function");
        }
        pc = function_starts[0];
    }

    while (pc < instruction_count) {
        const size_t offset = MILENA_BYTECODE_HEADER_SIZE +
                              (size_t)pc * MILENA_BYTECODE_INSTRUCTION_SIZE;
        const uint8_t opcode = bytes[offset];
        uint32_t a;
        uint32_t b;
        uint32_t c;
        double immediate;
        double left;
        double right;
        double value;

        if (steps >= resolved.max_steps) {
            free(registers);
            return fail(diagnostic, MILENA_BC_STEP_LIMIT, offset,
                        "execution instruction budget exhausted");
        }
        ++steps;
        (void)read_instruction(bytes, pc, &a, &b, &c, &immediate);
        switch (opcode) {
            case MILENA_BC_CONST_F64:
            case MILENA_BC_CONST_BOOL:
                registers[a] = immediate;
                ++pc;
                break;
            case MILENA_BC_MOVE:
                registers[a] = registers[b];
                ++pc;
                break;
            case MILENA_BC_NEG:
                value = -registers[b];
                if (!isfinite(value)) {
                    free(registers);
                    return fail(diagnostic, MILENA_BC_RUNTIME_ERROR, offset,
                                "non-finite numeric result");
                }
                registers[a] = value;
                ++pc;
                break;
            case MILENA_BC_ADD:
            case MILENA_BC_SUB:
            case MILENA_BC_MUL:
            case MILENA_BC_DIV:
            case MILENA_BC_EQ:
            case MILENA_BC_NE:
            case MILENA_BC_LT:
            case MILENA_BC_LE:
            case MILENA_BC_GT:
            case MILENA_BC_GE:
                left = registers[b];
                right = registers[c];
                if (opcode == MILENA_BC_DIV && right == 0.0) {
                    free(registers);
                    return fail(diagnostic, MILENA_BC_RUNTIME_ERROR, offset,
                                "division by zero");
                }
                switch (opcode) {
                    case MILENA_BC_ADD: value = left + right; break;
                    case MILENA_BC_SUB: value = left - right; break;
                    case MILENA_BC_MUL: value = left * right; break;
                    case MILENA_BC_DIV: value = left / right; break;
                    case MILENA_BC_EQ: value = left == right ? 1.0 : 0.0; break;
                    case MILENA_BC_NE: value = left != right ? 1.0 : 0.0; break;
                    case MILENA_BC_LT: value = left < right ? 1.0 : 0.0; break;
                    case MILENA_BC_LE: value = left <= right ? 1.0 : 0.0; break;
                    case MILENA_BC_GT: value = left > right ? 1.0 : 0.0; break;
                    case MILENA_BC_GE: value = left >= right ? 1.0 : 0.0; break;
                    default:
                        free(registers);
                        return fail(diagnostic, MILENA_BC_RUNTIME_ERROR, offset,
                                    "verified opcode is not executable");
                }
                if (!isfinite(value)) {
                    free(registers);
                    return fail(diagnostic, MILENA_BC_RUNTIME_ERROR, offset,
                                "non-finite numeric result");
                }
                registers[a] = value;
                ++pc;
                break;
            case MILENA_BC_JUMP:
                pc = a;
                break;
            case MILENA_BC_JUMP_IF_FALSE:
                pc = registers[a] == 0.0 ? b : pc + 1;
                break;
            case MILENA_BC_RETURN:
                value = registers[a];
                if (frame_depth == 0) {
                    *result = value;
                    free(registers);
                    clear_diagnostic(diagnostic);
                    return MILENA_BC_OK;
                }
                --frame_depth;
                registers[frames[frame_depth].destination] = value;
                pc = frames[frame_depth].return_pc;
                break;
            case MILENA_BC_FUNCTION:
                ++pc;
                break;
            case MILENA_BC_CALL: {
                uint32_t arity = (uint32_t)immediate;
                double args[MILENA_BYTECODE_MAX_REGISTERS];
                if (frame_depth >= resolved.max_call_depth) {
                    free(registers);
                    return fail(diagnostic, MILENA_BC_LIMIT_EXCEEDED, offset,
                                "runtime call-frame limit exhausted");
                }
                if (b >= function_count || arity != arities[b] ||
                    c > register_count || arity > register_count - c ||
                    parameter_bases[b] > register_count ||
                    arity > register_count - parameter_bases[b]) {
                    free(registers);
                    return fail(diagnostic, MILENA_BC_BAD_OPERAND, offset,
                                "runtime call target or frame range is invalid");
                }
                for (uint32_t i = 0; i < arity; ++i) args[i] = registers[c + i];
                frames[frame_depth].return_pc = pc + 1u;
                frames[frame_depth].destination = a;
                frame_depth++;
                for (uint32_t i = 0; i < arity; ++i)
                    registers[parameter_bases[b] + i] = args[i];
                pc = function_starts[b];
                break;
            }
            default:
                free(registers);
                return fail(diagnostic, MILENA_BC_RUNTIME_ERROR, offset,
                            "verified opcode is not executable");
        }
    }
    free(registers);
    return fail(diagnostic, MILENA_BC_RUNTIME_ERROR, length,
                "program counter left the verified instruction stream");
}

const char *milena_bytecode_status_name(MilenaBytecodeStatus status) {
    switch (status) {
        case MILENA_BC_OK: return "ok";
        case MILENA_BC_ARGUMENT: return "argument";
        case MILENA_BC_BUFFER_TOO_SMALL: return "buffer-too-small";
        case MILENA_BC_BAD_MAGIC: return "bad-magic";
        case MILENA_BC_BAD_VERSION: return "bad-version";
        case MILENA_BC_BAD_FORMAT: return "bad-format";
        case MILENA_BC_LIMIT_EXCEEDED: return "limit-exceeded";
        case MILENA_BC_BAD_OPCODE: return "bad-opcode";
        case MILENA_BC_BAD_OPERAND: return "bad-operand";
        case MILENA_BC_BAD_CONTROL_FLOW: return "bad-control-flow";
        case MILENA_BC_OUT_OF_MEMORY: return "out-of-memory";
        case MILENA_BC_RUNTIME_ERROR: return "runtime-error";
        case MILENA_BC_STEP_LIMIT: return "step-limit";
        case MILENA_BC_BAD_TYPE: return "bad-type";
        default: return "unknown-status";
    }
}
