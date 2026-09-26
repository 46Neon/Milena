#define _POSIX_C_SOURCE 200809L
#include "bytecode_compiler.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MILENA_BYTECODE_NATIVE_CC
#define MILENA_BYTECODE_NATIVE_CC "/usr/bin/cc"
#endif

#define NATIVE_STEP_LIMIT UINT64_C(1000000)

static void native_error(MilenaError *error, MilenaStatus status,
                         const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static bool target_supported(void) {
#if defined(__linux__) && defined(__x86_64__)
    return sizeof(double) == sizeof(uint64_t) && FLT_RADIX == 2 &&
           DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024;
#else
    return false;
#endif
}

typedef struct {
    uint32_t body, end, parameter_base, arity;
} NativeFunction;

static bool emit_call_program(FILE *out, const uint8_t *bytes) {
    const uint16_t registers = read_le16(bytes + 8);
    const uint32_t instructions = read_le32(bytes + 12);
    NativeFunction functions[MILENA_BYTECODE_MAX_FUNCTIONS] = {{0}};
    uint32_t count = 0;
    for (uint32_t pc = 0; pc < instructions; ++pc) {
        const uint8_t *ins = bytes + MILENA_BYTECODE_HEADER_SIZE +
                             (size_t)pc * MILENA_BYTECODE_INSTRUCTION_SIZE;
        if (ins[0] != MILENA_BC_FUNCTION) continue;
        uint32_t id = read_le32(ins + 4);
        if (id >= MILENA_BYTECODE_MAX_FUNCTIONS) return false;
        if (count) functions[count - 1u].end = pc;
        functions[id] = (NativeFunction){pc + 1u, instructions,
                                          read_le32(ins + 8), read_le32(ins + 12)};
        if (count <= id) count = id + 1u;
    }
    if (!count || functions[0].body >= functions[0].end) return false;
    if (fprintf(out,
                "#include <math.h>\n#include <stdint.h>\n#include <stdio.h>\n"
                "#include <string.h>\n"
                "static double fn_f64(uint64_t bits) { double v; memcpy(&v,&bits,sizeof v); return v; }\n") < 0)
        return false;
    for (uint32_t f = 0; f < count; ++f)
        if (fprintf(out, "static double fn_%" PRIu32 "(double*,uint64_t*,int*,uint32_t);\n", f) < 0)
            return false;
    for (uint32_t f = 0; f < count; ++f) {
        NativeFunction *function = &functions[f];
        if (fprintf(out,
                    "static double fn_%" PRIu32 "(double *r,uint64_t *steps,int *status,uint32_t argbase) {\n"
                    "  double args[%u] = {0.0}; double v;\n"
                    "  for (uint32_t j=0;j<%" PRIu32 ";++j) args[j]=r[argbase+j];\n",
                    f, function->arity ? function->arity : 1u, function->arity) < 0)
            return false;
        for (uint32_t i = 0; i < function->arity; ++i)
            if (fprintf(out, "  r[%" PRIu32 "+%" PRIu32 "]=args[%" PRIu32 "];\n",
                        function->parameter_base, i, i) < 0) return false;
        for (uint32_t pc = function->body; pc < function->end; ++pc) {
            const uint8_t *ins = bytes + MILENA_BYTECODE_HEADER_SIZE +
                                 (size_t)pc * MILENA_BYTECODE_INSTRUCTION_SIZE;
            uint8_t op = ins[0];
            uint32_t a = read_le32(ins + 4), b = read_le32(ins + 8), c = read_le32(ins + 12);
            uint64_t immediate = read_le64(ins + 16);
            if (fprintf(out,
                        "bc_%" PRIu32 ":\n"
                        "  if (*steps >= UINT64_C(1000000)) { *status=71; return 0.0; } ++*steps;\n",
                        pc) < 0) return false;
            switch (op) {
                case MILENA_BC_CONST_F64:
                    if (fprintf(out, "  r[%" PRIu32 "]=fn_f64(UINT64_C(0x%016" PRIx64 "));\n", a, immediate) < 0) return false;
                    break;
                case MILENA_BC_MOVE:
                    if (fprintf(out, "  r[%" PRIu32 "]=r[%" PRIu32 "];\n", a, b) < 0) return false;
                    break;
                case MILENA_BC_NEG:
                    if (fprintf(out, "  v=-r[%" PRIu32 "]; if(!isfinite(v)){*status=70;return 0.0;} r[%" PRIu32 "]=v;\n", b, a) < 0) return false;
                    break;
                case MILENA_BC_ADD: case MILENA_BC_SUB: case MILENA_BC_MUL: case MILENA_BC_DIV:
                case MILENA_BC_EQ: case MILENA_BC_NE: case MILENA_BC_LT: case MILENA_BC_LE:
                case MILENA_BC_GT: case MILENA_BC_GE: {
                    const char *operator = op == MILENA_BC_ADD ? "+" : op == MILENA_BC_SUB ? "-" :
                        op == MILENA_BC_MUL ? "*" : op == MILENA_BC_DIV ? "/" :
                        op == MILENA_BC_EQ ? "==" : op == MILENA_BC_NE ? "!=" :
                        op == MILENA_BC_LT ? "<" : op == MILENA_BC_LE ? "<=" :
                        op == MILENA_BC_GT ? ">" : ">=";
                    if (op == MILENA_BC_DIV && fprintf(out, "  if(r[%" PRIu32 "]==0.0){*status=70;return 0.0;}\n", c) < 0) return false;
                    if (op >= MILENA_BC_EQ) {
                        if (fprintf(out, "  r[%" PRIu32 "]=r[%" PRIu32 "] %s r[%" PRIu32 "] ? 1.0 : 0.0;\n", a, b, operator, c) < 0) return false;
                    } else if (fprintf(out, "  v=r[%" PRIu32 "] %s r[%" PRIu32 "]; if(!isfinite(v)){*status=70;return 0.0;} r[%" PRIu32 "]=v;\n", b, operator, c, a) < 0) return false;
                    break;
                }
                case MILENA_BC_JUMP:
                    if (fprintf(out, "  goto bc_%" PRIu32 ";\n", a) < 0) return false;
                    continue;
                case MILENA_BC_JUMP_IF_FALSE:
                    if (fprintf(out, "  if(r[%" PRIu32 "]==0.0) goto bc_%" PRIu32 "; ", a, b) < 0) return false;
                    if (pc + 1u < function->end) {
                        if (fprintf(out, "goto bc_%" PRIu32 ";\n", pc + 1u) < 0) return false;
                    } else if (fprintf(out, "*status=70; return 0.0;\n") < 0) return false;
                    continue;
                case MILENA_BC_RETURN:
                    if (fprintf(out, "  return r[%" PRIu32 "];\n", a) < 0) return false;
                    continue;
                case MILENA_BC_CALL:
                    if (b >= count || fprintf(out,
                        "  r[%" PRIu32 "]=fn_%" PRIu32 "(r,steps,status,%" PRIu32 "); if(*status) return 0.0;\n",
                        a, b, c) < 0) return false;
                    break;
                default: return false;
            }
            if (pc + 1u < function->end) {
                if (fprintf(out, "  goto bc_%" PRIu32 ";\n", pc + 1u) < 0) return false;
            } else if (fprintf(out, "  *status=70; return 0.0;\n") < 0) return false;
        }
        if (fprintf(out, "  *status=70; return 0.0;\n}\n") < 0) return false;
    }
    if (fprintf(out,
                "int main(void){double r[%u]={0.0};uint64_t steps=0;int status=0;"
                "double value=fn_0(r,&steps,&status,0);if(status)return status;"
                "if(printf(\"%%a\\n\",value)<0||fflush(stdout)!=0)return 74;return 0;}\n",
                (unsigned)registers) < 0) return false;
    return !ferror(out);
}

static bool emit_program(FILE *out, const uint8_t *bytes) {
    if (read_le16(bytes + 6) == MILENA_BYTECODE_VERSION_CALL_MINOR)
        return emit_call_program(out, bytes);
    const uint16_t registers = read_le16(bytes + 8);
    const uint32_t instructions = read_le32(bytes + 12);
    if (fprintf(out,
                "#include <math.h>\n#include <stdint.h>\n"
                "#include <stdio.h>\n#include <string.h>\n"
                "static double f64(uint64_t bits) { double value; "
                "memcpy(&value, &bits, sizeof value); return value; }\n"
                "int main(void) {\n"
                "  double r[%u] = {0.0};\n"
                "  uint64_t steps = 0;\n"
                "  double v;\n"
                "  goto bc_0;\n", (unsigned)registers) < 0) return false;

    for (uint32_t i = 0; i < instructions; ++i) {
        const uint8_t *ins = bytes + MILENA_BYTECODE_HEADER_SIZE +
                             (size_t)i * MILENA_BYTECODE_INSTRUCTION_SIZE;
        const uint8_t op = ins[0];
        const uint32_t a = read_le32(ins + 4);
        const uint32_t b = read_le32(ins + 8);
        const uint32_t c = read_le32(ins + 12);
        const uint64_t immediate = read_le64(ins + 16);
        if (fprintf(out,
                    "bc_%" PRIu32 ":\n"
                    "  if (steps >= UINT64_C(%" PRIu64 ")) return 71;\n"
                    "  ++steps;\n", i, NATIVE_STEP_LIMIT) < 0) return false;
        switch (op) {
            case MILENA_BC_CONST_F64:
                if (fprintf(out, "  r[%" PRIu32 "] = f64(UINT64_C(0x%016" PRIx64 "));\n",
                            a, immediate) < 0) return false;
                break;
            case MILENA_BC_MOVE:
                if (fprintf(out, "  r[%" PRIu32 "] = r[%" PRIu32 "];\n", a, b) < 0)
                    return false;
                break;
            case MILENA_BC_NEG:
                if (fprintf(out,
                            "  v = -r[%" PRIu32 "];\n"
                            "  if (!isfinite(v)) return 70;\n"
                            "  r[%" PRIu32 "] = v;\n", b, a) < 0) return false;
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
            case MILENA_BC_GE: {
                const char *operator = "==";
                switch (op) {
                    case MILENA_BC_ADD: operator = "+"; break;
                    case MILENA_BC_SUB: operator = "-"; break;
                    case MILENA_BC_MUL: operator = "*"; break;
                    case MILENA_BC_DIV: operator = "/"; break;
                    case MILENA_BC_EQ: operator = "=="; break;
                    case MILENA_BC_NE: operator = "!="; break;
                    case MILENA_BC_LT: operator = "<"; break;
                    case MILENA_BC_LE: operator = "<="; break;
                    case MILENA_BC_GT: operator = ">"; break;
                    case MILENA_BC_GE: operator = ">="; break;
                    default: return false;
                }
                if (op == MILENA_BC_DIV &&
                    fprintf(out, "  if (r[%" PRIu32 "] == 0.0) return 70;\n", c) < 0)
                    return false;
                if (op >= MILENA_BC_EQ) {
                    if (fprintf(out,
                                "  r[%" PRIu32 "] = r[%" PRIu32 "] %s r[%" PRIu32 "] ? 1.0 : 0.0;\n",
                                a, b, operator, c) < 0) return false;
                } else if (fprintf(out,
                                   "  v = r[%" PRIu32 "] %s r[%" PRIu32 "];\n"
                                   "  if (!isfinite(v)) return 70;\n"
                                   "  r[%" PRIu32 "] = v;\n",
                                   b, operator, c, a) < 0) return false;
                break;
            }
            case MILENA_BC_JUMP:
                if (fprintf(out, "  goto bc_%" PRIu32 ";\n", a) < 0) return false;
                continue;
            case MILENA_BC_JUMP_IF_FALSE:
                if (fprintf(out,
                            "  if (r[%" PRIu32 "] == 0.0) goto bc_%" PRIu32 ";\n"
                            "  goto bc_%" PRIu32 ";\n", a, b, i + 1u) < 0)
                    return false;
                continue;
            case MILENA_BC_RETURN:
                if (fprintf(out,
                            "  if (printf(\"%%a\\n\", r[%" PRIu32 "]) < 0 || fflush(stdout) != 0) return 74;\n"
                            "  return 0;\n", a) < 0) return false;
                continue;
            default:
                return false;
        }
        if (fprintf(out, "  goto bc_%" PRIu32 ";\n", i + 1u) < 0) return false;
    }
    if (fprintf(out, "bc_%" PRIu32 ": return 70;\n}\n", instructions) < 0)
        return false;
    return !ferror(out);
}

static bool write_source(FILE *out, const uint8_t *bytes) {
    bool ok = emit_program(out, bytes);
    if (fclose(out) != 0) ok = false;
    return ok;
}

static int invoke_fixed_compiler(const char *source_path, const char *output_path) {
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        char *const argv[] = {
            (char *)MILENA_BYTECODE_NATIVE_CC,
            (char *)"-std=c11", (char *)"-O2", (char *)"-fno-fast-math",
            (char *)"-x", (char *)"c", (char *)source_path,
            (char *)"-lm", (char *)"-o", (char *)output_path, NULL
        };
        char *const envp[] = {(char *)"LC_ALL=C", (char *)"PATH=/usr/bin:/bin", NULL};
        execve(MILENA_BYTECODE_NATIVE_CC, argv, envp);
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 126;
}

MilenaStatus milena_bytecode_compile_native(const uint8_t *bytes,
                                             size_t length,
                                             const char *executable_path,
                                             MilenaError *error) {
    char source_path[] = "/tmp/milena-aot-source-XXXXXX";
    char *temporary_executable = NULL;
    size_t output_length;
    int source_fd = -1;
    int output_fd = -1;
    bool source_created = false;
    bool output_created = false;
    MilenaBytecodeDiagnostic diagnostic;
    MilenaStatus result = MILENA_ERR_IO;

    milena_error_clear(error);
    if (!bytes || !executable_path || executable_path[0] == '\0') {
        native_error(error, MILENA_ERR_ARGUMENT,
                     "native bytecode compilation requires bytes and an output path");
        return MILENA_ERR_ARGUMENT;
    }
    if (!target_supported()) {
        native_error(error, MILENA_ERR_UNSUPPORTED,
                     "native bytecode backend currently supports Linux x86-64 with IEEE-754 binary64 only");
        return MILENA_ERR_UNSUPPORTED;
    }
    if (milena_bytecode_verify(bytes, length, NULL, &diagnostic) != MILENA_BC_OK) {
        char message[MILENA_ERROR_TEXT];
        (void)snprintf(message, sizeof(message),
                       "refusing to compile invalid bytecode at offset %zu: %s",
                       diagnostic.byte_offset, diagnostic.message);
        native_error(error, MILENA_ERR_DATA, message);
        return MILENA_ERR_DATA;
    }

    size_t suffix_length = sizeof(".tmp.XXXXXX") - 1u;
    if (!milena_size_add(strlen(executable_path), suffix_length + 1u,
                         &output_length)) {
        native_error(error, MILENA_ERR_OVERFLOW, "native output path is too long");
        return MILENA_ERR_OVERFLOW;
    }
    temporary_executable = malloc(output_length);
    if (!temporary_executable) {
        native_error(error, MILENA_ERR_MEMORY, "unable to allocate native output path");
        return MILENA_ERR_MEMORY;
    }
    (void)snprintf(temporary_executable, output_length, "%s.tmp.XXXXXX",
                   executable_path);
    output_fd = mkstemp(temporary_executable);
    if (output_fd < 0) {
        native_error(error, MILENA_ERR_IO, "unable to create a private native output temporary file");
        goto cleanup;
    }
    output_created = true;
    if (close(output_fd) != 0) {
        output_fd = -1;
        native_error(error, MILENA_ERR_IO, "unable to close native output temporary file");
        goto cleanup;
    }
    output_fd = -1;

    source_fd = mkstemp(source_path);
    if (source_fd < 0) {
        native_error(error, MILENA_ERR_IO, "unable to create a private generated-C temporary file");
        goto cleanup;
    }
    source_created = true;
    FILE *source = fdopen(source_fd, "w");
    if (!source) {
        native_error(error, MILENA_ERR_IO, "unable to open generated-C temporary file stream");
        goto cleanup;
    }
    source_fd = -1;
    if (!write_source(source, bytes)) {
        native_error(error, MILENA_ERR_IO, "unable to write generated C for verified bytecode");
        goto cleanup;
    }

    int compiler_status = invoke_fixed_compiler(source_path, temporary_executable);
    if (compiler_status != 0) {
        char message[MILENA_ERROR_TEXT];
        (void)snprintf(message, sizeof(message),
                       "fixed native compiler failed with status %d", compiler_status);
        native_error(error, MILENA_ERR_IO, message);
        goto cleanup;
    }
    if (chmod(temporary_executable, S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
        native_error(error, MILENA_ERR_IO, "unable to mark native output executable");
        goto cleanup;
    }
    if (rename(temporary_executable, executable_path) != 0) {
        native_error(error, MILENA_ERR_IO, "unable to publish compiled native executable");
        goto cleanup;
    }
    output_created = false;
    result = MILENA_OK;
    milena_error_clear(error);

cleanup:
    if (source_fd >= 0) (void)close(source_fd);
    if (output_fd >= 0) (void)close(output_fd);
    if (source_created) (void)unlink(source_path);
    if (output_created) (void)unlink(temporary_executable);
    free(temporary_executable);
    return result;
}
