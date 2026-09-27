#define _POSIX_C_SOURCE 200809L

#include "native_aot.h"

#include "canonical_compiler.h"
#include "typed_ir.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define MILENA_AOT_SOURCE_LIMIT (16u * 1024u * 1024u)
#define ELF64_HEADER_SIZE 64u
#define ELF64_PROGRAM_HEADER_SIZE 56u
#define ELF64_CODE_OFFSET (ELF64_HEADER_SIZE + ELF64_PROGRAM_HEADER_SIZE)
#define ELF64_LOAD_ADDRESS UINT64_C(0x400000)

typedef struct {
    const char *runtime_error;
} AOTSubsetAnalysis;

static void aot_error(MilenaError *error, MilenaStatus status,
                      const char *message) {
    if (error)
        milena_error_set(error, status, 0, 0, 0, message);
}

static MilenaStatus aot_unsupported(MilenaError *error, const char *message) {
    aot_error(error, MILENA_ERR_UNSUPPORTED, message);
    return MILENA_ERR_UNSUPPORTED;
}

static MilenaStatus read_source(const char *filename, char **source,
                                MilenaError *error) {
    *source = NULL;
    FILE *file = fopen(filename, "rb");
    if (!file) {
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo abrir el archivo fuente Milena");
        return MILENA_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo medir el archivo fuente Milena");
        return MILENA_ERR_IO;
    }
    long end = ftell(file);
    if (end < 0) {
        (void)fclose(file);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo medir el archivo fuente Milena");
        return MILENA_ERR_IO;
    }
    if ((unsigned long)end > MILENA_AOT_SOURCE_LIMIT) {
        (void)fclose(file);
        aot_error(error, MILENA_ERR_UNSUPPORTED,
                  "El archivo fuente excede el límite de 16 MiB");
        return MILENA_ERR_UNSUPPORTED;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo volver al inicio del archivo fuente Milena");
        return MILENA_ERR_IO;
    }
    size_t length = (size_t)end;
    if (length == SIZE_MAX) {
        (void)fclose(file);
        aot_error(error, MILENA_ERR_OVERFLOW,
                  "El tamaño de la fuente excede el espacio direccionable");
        return MILENA_ERR_OVERFLOW;
    }
    char *buffer = malloc(length + 1);
    if (!buffer) {
        (void)fclose(file);
        aot_error(error, MILENA_ERR_MEMORY,
                  "Sin memoria para leer el archivo fuente Milena");
        return MILENA_ERR_MEMORY;
    }
    size_t read_count = fread(buffer, 1, length, file);
    bool failed = read_count != length || ferror(file);
    if (fclose(file) != 0) failed = true;
    if (failed) {
        free(buffer);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo leer completamente el archivo fuente Milena");
        return MILENA_ERR_IO;
    }
    if (memchr(buffer, '\0', length) != NULL) {
        free(buffer);
        aot_error(error, MILENA_ERR_PARSE,
                  "El archivo fuente contiene un byte NUL no válido");
        return MILENA_ERR_PARSE;
    }
    buffer[length] = '\0';
    *source = buffer;
    return MILENA_OK;
}

static bool source_output_alias(const char *source_filename,
                                const char *output_filename) {
#if defined(__linux__)
    struct stat source_stat;
    struct stat output_stat;
    if (stat(source_filename, &source_stat) != 0) return false;
    if (stat(output_filename, &output_stat) != 0) return false;
    return source_stat.st_dev == output_stat.st_dev &&
           source_stat.st_ino == output_stat.st_ino;
#else
    (void)source_filename;
    (void)output_filename;
    return false;
#endif
}

static bool find_entry_function(const MilenaCanonicalProgram *program,
                                const MilenaIRProgram **body) {
    if (!program || !program->hir || program->hir->statement_count != 0 ||
        !program->hir->functions || program->hir->function_count != 1) return false;
    const MilenaHIRFunction *hir = &program->hir->functions[0];
    if (!hir->name || strcmp(hir->name, "principal") != 0 ||
        hir->parameter_count != 0) return false;
    if (program->typed_module) {
        if (!program->typed_module->functions ||
            program->typed_module->function_count != 1 ||
            !milena_ir_module_validate(program->typed_module, NULL, 0)) return false;
        const MilenaIRModuleFunction *function =
            &program->typed_module->functions[0];
        if (!function->name || strcmp(function->name, "principal") != 0 ||
            function->parameter_count != 0 ||
            function->return_type != MILENA_IR_TYPE_F64 || !function->body)
            return false;
        *body = function->body;
    } else {
        if (!program->typed_ir || !program->typed_ir->has_function_signature ||
            program->typed_ir->signature.parameter_count != 0 ||
            program->typed_ir->signature.return_type != MILENA_IR_TYPE_F64)
            return false;
        *body = program->typed_ir;
    }
    return true;
}

/* This backend accepts only verified single-block scalar IR whose exact
 * integer-valued F64 operations can be represented without rounding. The
 * values below are used only to prove the subset safe; output arithmetic is
 * emitted as SSE2 instructions and runs in the generated executable. */
static bool evaluate_verified_subset(const MilenaIRProgram *ir,
                                     AOTSubsetAnalysis *result,
                                     char *diagnostic, size_t capacity) {
    if (!ir || !result || !ir->has_function_signature ||
        ir->signature.return_type != MILENA_IR_TYPE_F64 ||
        ir->signature.parameter_count != 0 || ir->parameter_count != 0 ||
        ir->block_count != 1 || !ir->blocks || !ir->instructions ||
        ir->blocks[0].instruction_count != ir->count ||
        ir->blocks[0].first_instruction != 0 || ir->edge_argument_count != 0 ||
        ir->count == 0 || ir->count > 100000) {
        (void)snprintf(diagnostic, capacity,
                       "El backend ELF directo solo admite principal() constante en un bloque");
        return false;
    }
    char validation_error[MILENA_ERROR_TEXT] = {0};
    if (!milena_ir_program_validate(ir, validation_error,
                                    sizeof(validation_error))) {
        (void)snprintf(diagnostic, capacity, "%s",
                       validation_error[0] ? validation_error :
                       "El IR tipado no pasó su verificador");
        return false;
    }
    int64_t *values = calloc(ir->count + 1, sizeof(*values));
    bool *defined = calloc(ir->count + 1, sizeof(*defined));
    if (!values || !defined) {
        free(values);
        free(defined);
        (void)snprintf(diagnostic, capacity,
                       "Sin memoria para verificar el subconjunto ELF directo");
        return false;
    }
    const int64_t exact_limit = INT64_C(9007199254740992);
    bool returned = false;
    bool ok = true;
    for (size_t i = 0; i < ir->count && ok; ++i) {
        const MilenaIRInstruction *ins = &ir->instructions[i];
        if (ins->opcode == MILENA_IR_RETURN) {
            if (i + 1 != ir->count || ins->result_type != MILENA_IR_TYPE_F64 ||
                !ins->operand1_id || (size_t)ins->operand1_id > ir->count ||
                !defined[ins->operand1_id]) {
                ok = false;
                break;
            }
            returned = true;
            continue;
        }
        if (!ins->result_id || (size_t)ins->result_id > ir->count) {
            ok = false;
            break;
        }
        int64_t value = 0;
        if (ins->opcode == MILENA_IR_CONST_F64) {
            double literal = ins->float_immediate;
            if (ins->result_type != MILENA_IR_TYPE_F64 || !isfinite(literal) ||
                trunc(literal) != literal || fabs(literal) > (double)exact_limit ||
                (literal == 0.0 && signbit(literal))) {
                ok = false;
                break;
            }
            value = (int64_t)literal;
        } else if (ins->opcode == MILENA_IR_ADD_F64 ||
                   ins->opcode == MILENA_IR_SUB_F64 ||
                   ins->opcode == MILENA_IR_MUL_F64 ||
                   ins->opcode == MILENA_IR_DIV_F64) {
            if (ins->result_type != MILENA_IR_TYPE_F64 ||
                !ins->operand1_id || !ins->operand2_id ||
                (size_t)ins->operand1_id > ir->count ||
                (size_t)ins->operand2_id > ir->count ||
                !defined[ins->operand1_id] || !defined[ins->operand2_id]) {
                ok = false;
                break;
            }
            int64_t lhs = values[ins->operand1_id];
            int64_t rhs = values[ins->operand2_id];
            if (ins->opcode == MILENA_IR_DIV_F64 && rhs == 0) {
                if (i + 2 != ir->count ||
                    ir->instructions[i + 1].opcode != MILENA_IR_RETURN ||
                    ir->instructions[i + 1].operand1_id != ins->result_id) {
                    ok = false;
                    break;
                }
                result->runtime_error = "division by zero";
                free(values);
                free(defined);
                return true;
            }
            if (ins->opcode == MILENA_IR_ADD_F64) {
                value = lhs + rhs;
            } else if (ins->opcode == MILENA_IR_SUB_F64) {
                value = lhs - rhs;
            } else if (ins->opcode == MILENA_IR_MUL_F64) {
                int64_t abs_lhs = lhs < 0 ? -lhs : lhs;
                int64_t abs_rhs = rhs < 0 ? -rhs : rhs;
                if (abs_rhs != 0 && abs_lhs > exact_limit / abs_rhs) {
                    ok = false;
                    break;
                }
                value = lhs * rhs;
            } else {
                if (lhs % rhs != 0) {
                    ok = false;
                    break;
                }
                value = lhs / rhs;
            }
            /* Zero-producing arithmetic has signed-zero edge cases; reject it
             * rather than silently diverging from the reference formatter. */
            if (value == 0) {
                ok = false;
                break;
            }
            if (value < -exact_limit || value > exact_limit) {
                ok = false;
                break;
            }
        } else {
            ok = false;
            break;
        }
        values[ins->result_id] = value;
        defined[ins->result_id] = true;
    }
    free(values);
    free(defined);
    if (!ok || !returned) {
        (void)snprintf(diagnostic, capacity,
                       "El opcode, valor o flujo de control no pertenece al subconjunto entero exacto ELF directo");
        return false;
    }
    return true;
}

static void put_u16le(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_u32le(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

static void put_u64le(unsigned char *p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

static bool write_all(FILE *file, const void *data, size_t size) {
    const unsigned char *bytes = data;
    while (size) {
        size_t written = fwrite(bytes, 1, size, file);
        if (!written) return false;
        bytes += written;
        size -= written;
    }
    return true;
}

#if defined(__linux__) && defined(__x86_64__)
static MilenaStatus write_linux_x86_64_elf(const char *runtime_error,
                                           const char *output,
                                           MilenaError *error) {
    char output_text[64];
    int output_length = snprintf(output_text, sizeof(output_text),
                                 "Milena native runtime error: %s\n",
                                 runtime_error);
    const int exit_status = 70;
    const int file_descriptor = 2;
    if (output_length < 0 || (size_t)output_length >= sizeof(output_text)) {
        aot_error(error, MILENA_ERR_INTERNAL,
                  "No se pudo formatear el resultado para el artefacto ELF");
        return MILENA_ERR_INTERNAL;
    }
    const size_t text_length = 36;
    const size_t data_length = (size_t)output_length;
    if (data_length > UINT32_MAX || text_length > SIZE_MAX - ELF64_CODE_OFFSET ||
        data_length > SIZE_MAX - ELF64_CODE_OFFSET - text_length) {
        aot_error(error, MILENA_ERR_OVERFLOW,
                  "El artefacto ELF excede el espacio direccionable");
        return MILENA_ERR_OVERFLOW;
    }
    size_t file_size = ELF64_CODE_OFFSET + text_length + data_length;
    unsigned char header[ELF64_CODE_OFFSET] = {0};
    static const unsigned char ident[16] = {
        0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    memcpy(header, ident, sizeof(ident));
    put_u16le(header + 16, 2);   /* ET_EXEC */
    put_u16le(header + 18, 62);  /* EM_X86_64 */
    put_u32le(header + 20, 1);
    put_u64le(header + 24, ELF64_LOAD_ADDRESS + ELF64_CODE_OFFSET);
    put_u64le(header + 32, ELF64_HEADER_SIZE);
    put_u16le(header + 52, ELF64_HEADER_SIZE);
    put_u16le(header + 54, ELF64_PROGRAM_HEADER_SIZE);
    put_u16le(header + 56, 1);

    unsigned char *ph = header + ELF64_HEADER_SIZE;
    put_u32le(ph + 0, 1);       /* PT_LOAD */
    put_u32le(ph + 4, 5);       /* PF_R | PF_X */
    put_u64le(ph + 8, 0);
    put_u64le(ph + 16, ELF64_LOAD_ADDRESS);
    put_u64le(ph + 24, ELF64_LOAD_ADDRESS);
    put_u64le(ph + 32, (uint64_t)file_size);
    put_u64le(ph + 40, (uint64_t)file_size);
    put_u64le(ph + 48, 0x1000);

    unsigned char code[36] = {
        0xb8, 1, 0, 0, 0,                  /* mov eax, SYS_write */
        0xbf, 0, 0, 0, 0,                  /* mov edi, fd */
        0x48, 0x8d, 0x35, 0, 0, 0, 0,     /* lea rsi, [rip + message] */
        0xba, 0, 0, 0, 0,                  /* mov edx, message length */
        0x0f, 0x05,                        /* syscall */
        0xb8, 60, 0, 0, 0,                 /* mov eax, SYS_exit */
        0xbf, 0, 0, 0, 0,                  /* mov edi, exit status */
        0x0f, 0x05                         /* syscall */
    };
    code[6] = (unsigned char)file_descriptor;
    put_u32le(code + 13, (uint32_t)(text_length - 17));
    put_u32le(code + 18, (uint32_t)data_length);
    code[30] = (unsigned char)exit_status;

    const char *slash = strrchr(output, '/');
    size_t directory_length = slash ? (size_t)(slash - output + 1) : 0;
    const char *prefix = ".milena-elf-";
    size_t template_length = directory_length + (directory_length ? 0u : 2u) +
                             strlen(prefix) + sizeof("XXXXXX");
    char *temporary = malloc(template_length);
    if (!temporary) {
        aot_error(error, MILENA_ERR_MEMORY,
                  "Sin memoria para preparar el archivo temporal ELF");
        return MILENA_ERR_MEMORY;
    }
    size_t at = 0;
    if (directory_length) {
        memcpy(temporary, output, directory_length);
        at += directory_length;
    } else {
        temporary[at++] = '.';
        temporary[at++] = '/';
    }
    size_t prefix_length = strlen(prefix);
    memcpy(temporary + at, prefix, prefix_length);
    at += prefix_length;
    memcpy(temporary + at, "XXXXXX", sizeof("XXXXXX"));
    int fd = mkstemp(temporary);
    if (fd < 0) {
        free(temporary);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo crear el archivo temporal del ejecutable ELF");
        return MILENA_ERR_IO;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        (void)close(fd);
        (void)unlink(temporary);
        free(temporary);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo abrir el archivo temporal del ejecutable ELF");
        return MILENA_ERR_IO;
    }
    bool ok = write_all(file, header, sizeof(header)) &&
              write_all(file, code, sizeof(code)) &&
              write_all(file, output_text, data_length) &&
              fflush(file) == 0;
    if (fclose(file) != 0) ok = false;
    if (ok && chmod(temporary, 0755) != 0) ok = false;
    if (ok && rename(temporary, output) != 0) ok = false;
    if (!ok) {
        (void)unlink(temporary);
        free(temporary);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo instalar atómicamente el ejecutable ELF directo");
        return MILENA_ERR_IO;
    }
    free(temporary);
    return MILENA_OK;
}
#endif

#if defined(__linux__) && defined(__x86_64__)
typedef struct {
    unsigned char *bytes;
    size_t length;
    size_t capacity;
    bool failed;
} AOTCode;

static void code_reserve(AOTCode *code, size_t extra) {
    if (code->failed || extra > SIZE_MAX - code->length) {
        code->failed = true;
        return;
    }
    size_t needed = code->length + extra;
    if (needed <= code->capacity) return;
    size_t capacity = code->capacity ? code->capacity : 256u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    unsigned char *grown = realloc(code->bytes, capacity);
    if (!grown) {
        code->failed = true;
        return;
    }
    code->bytes = grown;
    code->capacity = capacity;
}

static void code_u8(AOTCode *code, unsigned char value) {
    code_reserve(code, 1u);
    if (!code->failed) code->bytes[code->length++] = value;
}

static void code_u32(AOTCode *code, uint32_t value) {
    code_reserve(code, 4u);
    if (code->failed) return;
    for (unsigned i = 0; i < 4u; ++i)
        code->bytes[code->length++] = (unsigned char)(value >> (i * 8u));
}

static void code_u64(AOTCode *code, uint64_t value) {
    code_reserve(code, 8u);
    if (code->failed) return;
    for (unsigned i = 0; i < 8u; ++i)
        code->bytes[code->length++] = (unsigned char)(value >> (i * 8u));
}

static void code_disp32(AOTCode *code, int32_t displacement) {
    code_u32(code, (uint32_t)displacement);
}

static int32_t stack_slot_displacement(uint32_t id) {
    return -(int32_t)(id * UINT32_C(8));
}

static void emit_linux_formatter(AOTCode *code, int32_t buffer_end) {
    /* Convert signed integer RAX to decimal at the end of a stack buffer,
     * append newline, and write it with the Linux x86-64 write syscall. */
    static const unsigned char save_value[] = {0x48, 0x89, 0xc3};
    for (size_t i = 0; i < sizeof(save_value); ++i) code_u8(code, save_value[i]);
    code_u8(code, 0x48); code_u8(code, 0x8d); code_u8(code, 0xb5);
    code_disp32(code, buffer_end - 1);
    code_u8(code, 0xc6); code_u8(code, 0x85);
    code_disp32(code, buffer_end); code_u8(code, 0x0a);
    code_u8(code, 0x41); code_u8(code, 0xb8); code_u32(code, 0u); /* r8d=0 */
    code_u8(code, 0x48); code_u8(code, 0x85); code_u8(code, 0xdb); /* test rbx */
    code_u8(code, 0x79); size_t positive_branch = code->length; code_u8(code, 0);
    code_u8(code, 0x48); code_u8(code, 0xf7); code_u8(code, 0xdb); /* neg rbx */
    code_u8(code, 0x41); code_u8(code, 0xb8); code_u32(code, 1u); /* r8d=1 */
    size_t positive = code->length;
    if (!code->failed && positive_branch < code->length)
        code->bytes[positive_branch] =
            (unsigned char)(positive - (positive_branch + 1u));
    code_u8(code, 0x45); code_u8(code, 0x31); code_u8(code, 0xc9); /* r9d=0 */

    size_t loop = code->length;
    code_u8(code, 0x48); code_u8(code, 0x89); code_u8(code, 0xd8); /* rax=rbx */
    code_u8(code, 0x31); code_u8(code, 0xd2);                   /* xor edx,edx */
    code_u8(code, 0xb9); code_u32(code, 10u);                  /* ecx=10 */
    code_u8(code, 0x48); code_u8(code, 0xf7); code_u8(code, 0xf1); /* div rcx */
    code_u8(code, 0x80); code_u8(code, 0xc2); code_u8(code, 0x30); /* dl += '0' */
    code_u8(code, 0x88); code_u8(code, 0x16);                   /* [rsi]=dl */
    code_u8(code, 0x48); code_u8(code, 0xff); code_u8(code, 0xce); /* dec rsi */
    code_u8(code, 0x41); code_u8(code, 0xff); code_u8(code, 0xc1); /* inc r9d */
    code_u8(code, 0x48); code_u8(code, 0x89); code_u8(code, 0xc3); /* rbx=rax */
    code_u8(code, 0x48); code_u8(code, 0x85); code_u8(code, 0xdb); /* test rbx */
    code_u8(code, 0x75); size_t loop_branch = code->length; code_u8(code, 0);
    if (!code->failed && loop_branch < code->length)
        code->bytes[loop_branch] = (unsigned char)(loop - (loop_branch + 1u));
    code_u8(code, 0x45); code_u8(code, 0x85); code_u8(code, 0xc0); /* test r8d */
    code_u8(code, 0x74); size_t no_sign_branch = code->length; code_u8(code, 0);
    code_u8(code, 0xc6); code_u8(code, 0x06); code_u8(code, 0x2d); /* '-' */
    code_u8(code, 0x48); code_u8(code, 0xff); code_u8(code, 0xce); /* dec rsi */
    code_u8(code, 0x41); code_u8(code, 0xff); code_u8(code, 0xc1); /* inc r9d */
    size_t no_sign = code->length;
    if (!code->failed && no_sign_branch < code->length)
        code->bytes[no_sign_branch] =
            (unsigned char)(no_sign - (no_sign_branch + 1u));
    code_u8(code, 0x41); code_u8(code, 0xff); code_u8(code, 0xc1); /* newline */
    code_u8(code, 0x48); code_u8(code, 0x8d); code_u8(code, 0x76); code_u8(code, 0x01);
    code_u8(code, 0xb8); code_u32(code, 1u);                  /* SYS_write */
    code_u8(code, 0xbf); code_u32(code, 1u);                  /* stdout */
    code_u8(code, 0x44); code_u8(code, 0x89); code_u8(code, 0xca); /* edx=r9d */
    code_u8(code, 0x0f); code_u8(code, 0x05);
    code_u8(code, 0xb8); code_u32(code, 60u);                 /* SYS_exit */
    code_u8(code, 0x31); code_u8(code, 0xff);                 /* status 0 */
    code_u8(code, 0x0f); code_u8(code, 0x05);
}

static MilenaStatus write_linux_x86_64_runtime_elf(const MilenaIRProgram *ir,
                                                    const char *output,
                                                    MilenaError *error) {
    if (ir->count > (UINT32_MAX - 128u) / 8u) {
        aot_error(error, MILENA_ERR_OVERFLOW,
                  "El marco de pila del artefacto ELF excede el límite admitido");
        return MILENA_ERR_OVERFLOW;
    }
    uint32_t frame_size = (uint32_t)(ir->count * 8u + 64u);
    frame_size = (frame_size + 15u) & ~UINT32_C(15);
    AOTCode code = {0};
    code_u8(&code, 0x55);                                      /* push rbp */
    code_u8(&code, 0x48); code_u8(&code, 0x89); code_u8(&code, 0xe5);
    code_u8(&code, 0x48); code_u8(&code, 0x81); code_u8(&code, 0xec);
    code_u32(&code, frame_size);
    bool returned = false;
    for (size_t i = 0; i < ir->count && !code.failed; ++i) {
        const MilenaIRInstruction *ins = &ir->instructions[i];
        if (ins->opcode == MILENA_IR_RETURN) {
            int32_t source = stack_slot_displacement(ins->operand1_id);
            code_u8(&code, 0xf2); code_u8(&code, 0x0f); code_u8(&code, 0x10);
            code_u8(&code, 0x85); code_disp32(&code, source); /* xmm0=[rbp+disp] */
            code_u8(&code, 0xf2); code_u8(&code, 0x48);
            code_u8(&code, 0x0f); code_u8(&code, 0x2c); code_u8(&code, 0xc0);
            int64_t end_offset = -((int64_t)ir->count * INT64_C(8) + 1);
            emit_linux_formatter(&code, (int32_t)end_offset);
            returned = true;
            break;
        }
        int32_t destination = stack_slot_displacement(ins->result_id);
        if (ins->opcode == MILENA_IR_CONST_F64) {
            uint64_t bits = 0;
            memcpy(&bits, &ins->float_immediate, sizeof(bits));
            code_u8(&code, 0x48); code_u8(&code, 0xb8); code_u64(&code, bits);
            code_u8(&code, 0x48); code_u8(&code, 0x89); code_u8(&code, 0x85);
            code_disp32(&code, destination);
        } else {
            int32_t lhs = stack_slot_displacement(ins->operand1_id);
            int32_t rhs = stack_slot_displacement(ins->operand2_id);
            code_u8(&code, 0xf2); code_u8(&code, 0x0f); code_u8(&code, 0x10);
            code_u8(&code, 0x85); code_disp32(&code, lhs);
            code_u8(&code, 0xf2); code_u8(&code, 0x0f); code_u8(&code, 0x10);
            code_u8(&code, 0x8d); code_disp32(&code, rhs);
            code_u8(&code, 0xf2); code_u8(&code, 0x0f);
            if (ins->opcode == MILENA_IR_ADD_F64) code_u8(&code, 0x58);
            else if (ins->opcode == MILENA_IR_SUB_F64) code_u8(&code, 0x5c);
            else if (ins->opcode == MILENA_IR_MUL_F64) code_u8(&code, 0x59);
            else code_u8(&code, 0x5e);
            code_u8(&code, 0xc1);
            code_u8(&code, 0xf2); code_u8(&code, 0x0f); code_u8(&code, 0x11);
            code_u8(&code, 0x85); code_disp32(&code, destination);
        }
    }
    if (code.failed || !returned) {
        free(code.bytes);
        aot_error(error, code.failed ? MILENA_ERR_MEMORY : MILENA_ERR_INTERNAL,
                  code.failed ? "Sin memoria para emitir el código máquina ELF" :
                                "El IR verificado no contiene retorno" );
        return code.failed ? MILENA_ERR_MEMORY : MILENA_ERR_INTERNAL;
    }
    if (code.length > SIZE_MAX - ELF64_CODE_OFFSET) {
        free(code.bytes);
        aot_error(error, MILENA_ERR_OVERFLOW,
                  "El artefacto ELF excede el espacio direccionable");
        return MILENA_ERR_OVERFLOW;
    }
    size_t file_size = ELF64_CODE_OFFSET + code.length;
    unsigned char header[ELF64_CODE_OFFSET] = {0};
    static const unsigned char ident[16] = {
        0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    memcpy(header, ident, sizeof(ident));
    put_u16le(header + 16, 2);
    put_u16le(header + 18, 62);
    put_u32le(header + 20, 1);
    put_u64le(header + 24, ELF64_LOAD_ADDRESS + ELF64_CODE_OFFSET);
    put_u64le(header + 32, ELF64_HEADER_SIZE);
    put_u16le(header + 52, ELF64_HEADER_SIZE);
    put_u16le(header + 54, ELF64_PROGRAM_HEADER_SIZE);
    put_u16le(header + 56, 1);
    unsigned char *ph = header + ELF64_HEADER_SIZE;
    put_u32le(ph + 0, 1);
    put_u32le(ph + 4, 5);
    put_u64le(ph + 8, 0);
    put_u64le(ph + 16, ELF64_LOAD_ADDRESS);
    put_u64le(ph + 24, ELF64_LOAD_ADDRESS);
    put_u64le(ph + 32, (uint64_t)file_size);
    put_u64le(ph + 40, (uint64_t)file_size);
    put_u64le(ph + 48, 0x1000);

    const char *slash = strrchr(output, '/');
    size_t directory_length = slash ? (size_t)(slash - output + 1) : 0;
    const char *prefix = ".milena-elf-";
    size_t template_length = directory_length + (directory_length ? 0u : 2u) +
                             strlen(prefix) + sizeof("XXXXXX");
    char *temporary = malloc(template_length);
    if (!temporary) {
        free(code.bytes);
        aot_error(error, MILENA_ERR_MEMORY,
                  "Sin memoria para preparar el archivo temporal ELF");
        return MILENA_ERR_MEMORY;
    }
    size_t at = 0;
    if (directory_length) {
        memcpy(temporary, output, directory_length);
        at += directory_length;
    } else {
        temporary[at++] = '.';
        temporary[at++] = '/';
    }
    size_t prefix_length = strlen(prefix);
    memcpy(temporary + at, prefix, prefix_length);
    at += prefix_length;
    memcpy(temporary + at, "XXXXXX", sizeof("XXXXXX"));
    int fd = mkstemp(temporary);
    if (fd < 0) {
        free(code.bytes);
        free(temporary);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo crear el archivo temporal del ejecutable ELF");
        return MILENA_ERR_IO;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        (void)close(fd);
        (void)unlink(temporary);
        free(code.bytes);
        free(temporary);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo abrir el archivo temporal del ejecutable ELF");
        return MILENA_ERR_IO;
    }
    bool ok = write_all(file, header, sizeof(header)) &&
              write_all(file, code.bytes, code.length) && fflush(file) == 0;
    if (fclose(file) != 0) ok = false;
    if (ok && chmod(temporary, 0755) != 0) ok = false;
    if (ok && rename(temporary, output) != 0) ok = false;
    free(code.bytes);
    if (!ok) {
        (void)unlink(temporary);
        free(temporary);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo instalar atómicamente el ejecutable ELF directo");
        return MILENA_ERR_IO;
    }
    free(temporary);
    return MILENA_OK;
}
/* Direct-call SysV AMD64 lowering for verified straight-line F64 modules. */
typedef struct { size_t disp; size_t target; } AOTFixup;
typedef struct { AOTFixup *v; size_t n, cap; bool failed; } AOTFixups;

static size_t aot_find_symbol(const MilenaIRModule *m, uint32_t id) {
    for (size_t i=0; i<m->function_count; ++i)
        if (m->functions[i].symbol_id == id) return i;
    return SIZE_MAX;
}
static void aot_fixup(AOTFixups *f, size_t disp, size_t target) {
    if (f->n == f->cap) {
        size_t cap=f->cap ? f->cap*2u : 16u;
        if (cap < f->cap || cap > SIZE_MAX/sizeof(*f->v)) { f->failed=true; return; }
        AOTFixup *p=realloc(f->v,cap*sizeof(*p));
        if (!p) { f->failed=true; return; }
        f->v=p; f->cap=cap;
    }
    f->v[f->n++]=(AOTFixup){disp,target};
}

/* Abstract interpretation is only a safety proof; emitted native operations
 * still calculate every result at runtime. Each call is checked in its actual
 * statically-known argument context. */
static bool aot_eval_fn(const MilenaIRModule *m, size_t fi, const int64_t *args,
        size_t argc, int64_t *out, const char **runtime_error, bool *reachable,
        size_t depth, char *diag, size_t dcap) {
    const int64_t lim=INT64_C(9007199254740992);
    const MilenaIRModuleFunction *fn=&m->functions[fi];
    const MilenaIRProgram *ir=fn->body;
    if (ir->parameter_count > 100000u || ir->count > 100000u) {
        snprintf(diag,dcap,"La función excede el límite de valores AOT"); return false;
    }
    size_t value_limit=0;
    for (size_t i=0;i<ir->parameter_count;++i)
        if (ir->parameters[i].value_id>value_limit) value_limit=ir->parameters[i].value_id;
    for (size_t i=0;i<ir->count;++i)
        if (ir->instructions[i].result_id>value_limit) value_limit=ir->instructions[i].result_id;
    if (value_limit>100000u) {
        snprintf(diag,dcap,"La función excede el límite de valores AOT"); return false;
    }
    if (depth > m->function_count || argc != fn->parameter_count || argc > 8u ||
        ir->block_count != 1u || !ir->blocks || ir->blocks[0].first_instruction != 0u ||
        ir->blocks[0].instruction_count != ir->count || ir->edge_argument_count ||
        ir->count == 0u || ir->count > 100000u) {
        snprintf(diag,dcap,"La función excede el subconjunto AOT de un bloque/F64 con hasta 8 argumentos"); return false;
    }
    char v[MILENA_ERROR_TEXT]={0};
    if (!milena_ir_program_validate(ir,v,sizeof(v))) {
        snprintf(diag,dcap,"%s",v[0]?v:"El IR de función no pasó su verificador"); return false;
    }
    if (fn->return_type != MILENA_IR_TYPE_F64) {
        snprintf(diag,dcap,"El backend de llamadas directas solo admite retorno F64"); return false;
    }
    for (size_t i=0;i<argc;++i) if (fn->parameter_types[i]!=MILENA_IR_TYPE_F64) {
        snprintf(diag,dcap,"El backend de llamadas directas solo admite parámetros F64"); return false;
    }
    int64_t *val=calloc(value_limit+1u,sizeof(*val));
    bool *def=calloc(value_limit+1u,sizeof(*def));
    if (!val || !def) { free(val);free(def);snprintf(diag,dcap,"Sin memoria para verificar el módulo AOT");return false; }
    size_t pi=0; bool ok=false;
    for (size_t i=0;i<ir->parameter_count;++i) if (ir->parameters[i].block_id==ir->blocks[0].id) {
        const MilenaIRBlockParameter *p=&ir->parameters[i];
        if (pi>=argc || p->type!=MILENA_IR_TYPE_F64 || !p->value_id || p->value_id>value_limit) goto bad;
        val[p->value_id]=args[pi++];def[p->value_id]=true;
    }
    if (pi!=argc) goto bad;
    reachable[fi]=true;
    for (size_t i=0;i<ir->count;++i) {
        const MilenaIRInstruction *in=&ir->instructions[i];
        if (in->opcode==MILENA_IR_RETURN) {
            if (i+1u!=ir->count || in->result_type!=MILENA_IR_TYPE_F64 || !in->operand1_id ||
                in->operand1_id>value_limit || !def[in->operand1_id]) goto bad;
            *out=val[in->operand1_id];ok=true;goto done;
        }
        if (!in->result_id || in->result_id>value_limit || in->result_type!=MILENA_IR_TYPE_F64) goto bad;
        int64_t r=0;
        if (in->opcode==MILENA_IR_CONST_F64) {
            double x=in->float_immediate;
            if (!isfinite(x)||trunc(x)!=x||fabs(x)>(double)lim||(x==0.0&&signbit(x))) goto bad;
            r=(int64_t)x;
        } else if (in->opcode==MILENA_IR_CALL) {
            size_t ci=aot_find_symbol(m,(uint32_t)in->integer_immediate);
            if (in->call_argument_count>8u) {
                snprintf(diag,dcap,"AOT directo admite como máximo 8 argumentos F64 por llamada");
                goto done;
            }
            if (ci==SIZE_MAX ||
                in->call_argument_count!=m->functions[ci].parameter_count ||
                in->call_argument_offset>ir->call_argument_count ||
                in->call_argument_count>ir->call_argument_count-in->call_argument_offset) goto bad;
            int64_t ca[8]={0};
            for (size_t a=0;a<in->call_argument_count;++a) {
                uint32_t id=ir->call_arguments[in->call_argument_offset+a];
                if (!id||id>value_limit||!def[id]) goto bad;
                ca[a]=val[id];
            }
            if (!aot_eval_fn(m,ci,ca,in->call_argument_count,&r,runtime_error,
                             reachable,depth+1u,diag,dcap)) goto done;
            if (*runtime_error) { ok=true;goto done; }
        } else if (in->opcode==MILENA_IR_ADD_F64 || in->opcode==MILENA_IR_SUB_F64 ||
                   in->opcode==MILENA_IR_MUL_F64 || in->opcode==MILENA_IR_DIV_F64) {
            if (!in->operand1_id||!in->operand2_id||in->operand1_id>value_limit||
                in->operand2_id>value_limit||!def[in->operand1_id]||!def[in->operand2_id]) goto bad;
            int64_t a=val[in->operand1_id], b=val[in->operand2_id];
            if (in->opcode==MILENA_IR_DIV_F64 && b==0) { *runtime_error="division by zero";ok=true;goto done; }
            if (in->opcode==MILENA_IR_ADD_F64) r=a+b;
            else if (in->opcode==MILENA_IR_SUB_F64) r=a-b;
            else if (in->opcode==MILENA_IR_MUL_F64) {
                int64_t aa=a<0?-a:a, bb=b<0?-b:b;
                if (bb && aa>lim/bb) goto bad;
                r=a*b;
            } else { if (a%b) goto bad; r=a/b; }
            if (!r||r < -lim||r > lim) goto bad;
        } else goto bad;
        val[in->result_id]=r;def[in->result_id]=true;
    }
    goto bad;
bad:
    snprintf(diag,dcap,"El opcode, valor o flujo no pertenece al subconjunto entero exacto ELF directo");
done:
    free(val);free(def);return ok;
}

static size_t aot_max_id(const MilenaIRProgram *ir) {
    size_t max=0;
    for(size_t i=0;i<ir->count;++i) if(ir->instructions[i].result_id>max) max=ir->instructions[i].result_id;
    for(size_t i=0;i<ir->parameter_count;++i) if(ir->parameters[i].value_id>max) max=ir->parameters[i].value_id;
    return max;
}
static void aot_xmm(AOTCode *c, unsigned reg, uint32_t id, bool store) {
    code_u8(c,0xf2);code_u8(c,0x0f);code_u8(c,store?0x11:0x10);
    code_u8(c,(unsigned char)(0x85u|(reg<<3)));code_disp32(c,stack_slot_displacement(id));
}
static bool aot_emit_fn(AOTCode *c,AOTFixups *fx,const MilenaIRModule *m,size_t fi) {
    const MilenaIRModuleFunction *fn=&m->functions[fi]; const MilenaIRProgram *ir=fn->body;
    size_t max=aot_max_id(ir); if(max>100000u||max>(size_t)INT32_MAX/8u||fn->parameter_count>8u)return false;
    uint32_t frame=(uint32_t)(max*8u+64u);frame=(frame+15u)&~UINT32_C(15);
    code_u8(c,0x55);code_u8(c,0x48);code_u8(c,0x89);code_u8(c,0xe5);
    code_u8(c,0x48);code_u8(c,0x81);code_u8(c,0xec);code_u32(c,frame);
    size_t pi=0;
    for(size_t i=0;i<ir->parameter_count;++i)if(ir->parameters[i].block_id==ir->blocks[0].id)
        aot_xmm(c,(unsigned)pi++,ir->parameters[i].value_id,true);
    if(pi!=fn->parameter_count)return false;
    for(size_t i=0;i<ir->count&&!c->failed;++i){
        const MilenaIRInstruction *in=&ir->instructions[i];
        if(in->opcode==MILENA_IR_RETURN){aot_xmm(c,0,in->operand1_id,false);code_u8(c,0xc9);code_u8(c,0xc3);return !c->failed;}
        if(in->opcode==MILENA_IR_CONST_F64){uint64_t bits;memcpy(&bits,&in->float_immediate,8);code_u8(c,0x48);code_u8(c,0xb8);code_u64(c,bits);code_u8(c,0x48);code_u8(c,0x89);code_u8(c,0x85);code_disp32(c,stack_slot_displacement(in->result_id));}
        else if(in->opcode==MILENA_IR_CALL){
            size_t target=aot_find_symbol(m,(uint32_t)in->integer_immediate);if(target==SIZE_MAX||in->call_argument_count>8u)return false;
            for(size_t a=0;a<in->call_argument_count;++a)aot_xmm(c,(unsigned)a,ir->call_arguments[in->call_argument_offset+a],false);
            code_u8(c,0xe8);size_t disp=c->length;code_u32(c,0);aot_fixup(fx,disp,target+1u);aot_xmm(c,0,in->result_id,true);
        } else {
            aot_xmm(c,0,in->operand1_id,false);aot_xmm(c,1,in->operand2_id,false);
            code_u8(c,0xf2);code_u8(c,0x0f);
            if(in->opcode==MILENA_IR_ADD_F64)code_u8(c,0x58);else if(in->opcode==MILENA_IR_SUB_F64)code_u8(c,0x5c);else if(in->opcode==MILENA_IR_MUL_F64)code_u8(c,0x59);else if(in->opcode==MILENA_IR_DIV_F64)code_u8(c,0x5e);else return false;
            code_u8(c,0xc1);aot_xmm(c,0,in->result_id,true);
        }
    }
    return false;
}

static MilenaStatus aot_write_module_elf(const MilenaIRModule *m,size_t entry,const char *output,MilenaError *error){
    AOTCode c={0};AOTFixups fx={0};size_t *offset=calloc(m->function_count+1u,sizeof(*offset));
    if(!offset){aot_error(error,MILENA_ERR_MEMORY,"Sin memoria para relocalizar las llamadas AOT");return MILENA_ERR_MEMORY;}
    /* _start has an aligned formatter frame; before CALL, RSP is 16-byte aligned. */
    code_u8(&c,0x55);code_u8(&c,0x48);code_u8(&c,0x89);code_u8(&c,0xe5);code_u8(&c,0x48);code_u8(&c,0x83);code_u8(&c,0xec);code_u8(&c,72);
    code_u8(&c,0xe8);size_t d=c.length;code_u32(&c,0);aot_fixup(&fx,d,entry+1u);
    code_u8(&c,0xf2);code_u8(&c,0x48);code_u8(&c,0x0f);code_u8(&c,0x2c);code_u8(&c,0xc0);
    emit_linux_formatter(&c,-64);
    for(size_t i=0;i<m->function_count;++i){offset[i+1u]=c.length;if(!aot_emit_fn(&c,&fx,m,i)){c.failed=true;break;}}
    if(fx.failed)c.failed=true;
    for(size_t i=0;i<fx.n&&!c.failed;++i){AOTFixup f=fx.v[i];if(f.target>m->function_count||f.disp>c.length-4u){c.failed=true;break;}int64_t rel=(int64_t)offset[f.target]-(int64_t)(f.disp+4u);if(rel<INT32_MIN||rel>INT32_MAX){c.failed=true;break;}put_u32le(c.bytes+f.disp,(uint32_t)(int32_t)rel);}
    free(fx.v);free(offset);
    if(c.failed||c.length>SIZE_MAX-ELF64_CODE_OFFSET){free(c.bytes);aot_error(error,MILENA_ERR_OVERFLOW,"El módulo o las relocalizaciones exceden el límite ELF");return MILENA_ERR_OVERFLOW;}
    size_t size=ELF64_CODE_OFFSET+c.length;unsigned char h[ELF64_CODE_OFFSET]={0};
    static const unsigned char ident[16]={0x7f,'E','L','F',2,1,1,0,0,0,0,0,0,0,0,0};memcpy(h,ident,16);put_u16le(h+16,2);put_u16le(h+18,62);put_u32le(h+20,1);put_u64le(h+24,ELF64_LOAD_ADDRESS+ELF64_CODE_OFFSET);put_u64le(h+32,ELF64_HEADER_SIZE);put_u16le(h+52,64);put_u16le(h+54,56);put_u16le(h+56,1);
    unsigned char *ph=h+64;put_u32le(ph,1);put_u32le(ph+4,5);put_u64le(ph+16,ELF64_LOAD_ADDRESS);put_u64le(ph+24,ELF64_LOAD_ADDRESS);put_u64le(ph+32,size);put_u64le(ph+40,size);put_u64le(ph+48,0x1000);
    const char *slash=strrchr(output,'/');size_t dn=slash?(size_t)(slash-output+1):0;const char *prefix=".milena-elf-";size_t tn=dn+(dn?0u:2u)+strlen(prefix)+sizeof("XXXXXX");char *tmp=malloc(tn);
    if(!tmp){free(c.bytes);aot_error(error,MILENA_ERR_MEMORY,"Sin memoria para ELF temporal");return MILENA_ERR_MEMORY;}size_t at=0;if(dn){memcpy(tmp,output,dn);at=dn;}else{tmp[at++]='.';tmp[at++]='/';}memcpy(tmp+at,prefix,strlen(prefix));at+=strlen(prefix);memcpy(tmp+at,"XXXXXX",7);
    int fd=mkstemp(tmp);if(fd<0){free(c.bytes);free(tmp);aot_error(error,MILENA_ERR_IO,"No se pudo crear ELF temporal");return MILENA_ERR_IO;}FILE *f=fdopen(fd,"wb");if(!f){close(fd);unlink(tmp);free(c.bytes);free(tmp);aot_error(error,MILENA_ERR_IO,"No se pudo abrir ELF temporal");return MILENA_ERR_IO;}
    bool good=write_all(f,h,sizeof(h))&&write_all(f,c.bytes,c.length)&&fflush(f)==0;if(fclose(f)!=0)good=false;if(good&&chmod(tmp,0755)!=0)good=false;if(good&&rename(tmp,output)!=0)good=false;free(c.bytes);if(!good){unlink(tmp);free(tmp);aot_error(error,MILENA_ERR_IO,"No se pudo instalar ELF AOT atómicamente");return MILENA_ERR_IO;}free(tmp);return MILENA_OK;
}

static MilenaStatus build_typed_module_aot(MilenaCanonicalProgram *program,const char *output,MilenaError *error){
    const MilenaIRModule *m=program->typed_module;if(!m||m->function_count<2u||m->function_count>1024u)return aot_unsupported(error,"AOT de llamadas directas requiere principal() y al menos una función auxiliar");
    char diag[MILENA_ERROR_TEXT]={0};if(!milena_ir_module_validate(m,diag,sizeof(diag)))return aot_unsupported(error,diag[0]?diag:"El módulo IR no pasó el verificador");
    size_t entry=SIZE_MAX;for(size_t i=0;i<m->function_count;++i)if(!strcmp(m->functions[i].name,"principal"))entry=i;
    if(entry==SIZE_MAX||m->functions[entry].parameter_count||m->functions[entry].return_type!=MILENA_IR_TYPE_F64)return aot_unsupported(error,"AOT directo requiere principal() sin argumentos y retorno F64");
    bool *reachable=calloc(m->function_count,sizeof(*reachable));if(!reachable){aot_error(error,MILENA_ERR_MEMORY,"Sin memoria para verificar llamadas AOT");return MILENA_ERR_MEMORY;}
    int64_t result=0;const char *runtime_error=NULL;bool ok=aot_eval_fn(m,entry,NULL,0,&result,&runtime_error,reachable,0,diag,sizeof(diag));
    if(ok)for(size_t i=0;i<m->function_count;++i)if(!reachable[i]){snprintf(diag,sizeof(diag),"El módulo contiene una función no alcanzable desde principal()");ok=false;break;}
    free(reachable);if(!ok)return aot_unsupported(error,diag[0]?diag:"El módulo excede el subconjunto AOT directo");
    if(runtime_error)return write_linux_x86_64_elf(runtime_error,output,error);
    return aot_write_module_elf(m,entry,output,error);
}

#endif

static MilenaStatus build_from_typed_ir(MilenaCanonicalProgram *program,
                                        const char *output_filename,
                                        MilenaError *error) {
#if !defined(__linux__) || !defined(__x86_64__)
    (void)program;
    (void)output_filename;
    return aot_unsupported(error,
        "El backend nativo directo solo está implementado para Linux x86-64 ELF");
#else
    if (program->typed_module && program->typed_module->function_count > 1u)
        return build_typed_module_aot(program, output_filename, error);
    const MilenaIRProgram *ir = NULL;
    if (!find_entry_function(program, &ir))
        return aot_unsupported(error,
            "AOT directo requiere un único principal() sin argumentos y retorno F64");
    AOTSubsetAnalysis result = {NULL};
    char diagnostic[MILENA_ERROR_TEXT] = {0};
    if (!evaluate_verified_subset(ir, &result, diagnostic, sizeof(diagnostic)))
        return aot_unsupported(error, diagnostic[0] ? diagnostic :
                               "El programa excede el subconjunto ELF directo");
    if (result.runtime_error)
        return write_linux_x86_64_elf(result.runtime_error, output_filename, error);
    return write_linux_x86_64_runtime_elf(ir, output_filename, error);
#endif
}

MilenaStatus milena_cli_build(const char *source_filename,
                              const char *output_filename,
                              MilenaError *error) {
    if (error) milena_error_clear(error);
    if (!source_filename || !source_filename[0] || !output_filename ||
        !output_filename[0]) {
        aot_error(error, MILENA_ERR_ARGUMENT,
                  "milena build requiere un archivo fuente y una salida");
        return MILENA_ERR_ARGUMENT;
    }
#if !defined(__linux__) || !defined(__x86_64__)
    return aot_unsupported(error,
        "El backend nativo directo solo está implementado para Linux x86-64 ELF");
#else
    if (source_output_alias(source_filename, output_filename)) {
        aot_error(error, MILENA_ERR_ARGUMENT,
                  "La fuente y el ejecutable de salida no pueden ser el mismo archivo");
        return MILENA_ERR_ARGUMENT;
    }
    char *source = NULL;
    MilenaStatus status = read_source(source_filename, &source, error);
    if (status != MILENA_OK) return status;
    MilenaCanonicalProgram program;
    milena_canonical_program_init(&program);
    status = milena_canonical_program_parse(&program, source, error);
    free(source);
    if (status == MILENA_OK)
        status = milena_canonical_program_compile_scalar_ir(&program, error);
    if (status == MILENA_OK)
        status = build_from_typed_ir(&program, output_filename, error);
    milena_canonical_program_release(&program);
    return status;
#endif
}
