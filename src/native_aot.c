#define _POSIX_C_SOURCE 200809L

#include "native_aot.h"

#include "canonical_compiler.h"
#include "typed_ir.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define MILENA_AOT_SOURCE_LIMIT (16u * 1024u * 1024u)

typedef struct {
    const char *name;
    uint32_t symbol_id;
    const MilenaIRProgram *ir;
} AOTFunction;

typedef struct {
    AOTFunction *functions;
    size_t function_count;
    size_t entry_index;
    const MilenaIRModule *module;
} AOTContext;

static void aot_error(MilenaError *error, MilenaStatus status,
                      const char *message) {
    if (error)
        milena_error_set(error, status, 0, 0, 0, message);
}

static MilenaStatus aot_unsupported(MilenaError *error, const char *message) {
    aot_error(error, MILENA_ERR_UNSUPPORTED, message);
    return MILENA_ERR_UNSUPPORTED;
}

static void aot_context_release(AOTContext *context) {
    if (!context) return;
    free(context->functions);
    memset(context, 0, sizeof(*context));
}

static const MilenaIRModuleFunction *module_find_function(
    const MilenaIRModule *module, uint32_t symbol_id) {
    if (!module || !symbol_id) return NULL;
    for (size_t i = 0; i < module->function_count; ++i)
        if (module->functions[i].symbol_id == symbol_id)
            return &module->functions[i];
    return NULL;
}

static const AOTFunction *context_find_function(const AOTContext *context,
                                                uint32_t symbol_id) {
    if (!context || !symbol_id) return NULL;
    for (size_t i = 0; i < context->function_count; ++i)
        if (context->functions[i].symbol_id == symbol_id)
            return &context->functions[i];
    return NULL;
}

static bool type_supported_value(MilenaIRType type) {
    return type == MILENA_IR_TYPE_F64 || type == MILENA_IR_TYPE_BOOL;
}

static bool validate_hir_signature(const MilenaHIRFunction *source,
                                   const MilenaIRProgram *typed) {
    if (!source || !typed || !typed->has_function_signature ||
        source->parameter_count != typed->signature.parameter_count ||
        (source->parameter_count &&
         (!source->parameters || !typed->signature.parameter_types))) return false;
    for (size_t i = 0; i < source->parameter_count; ++i) {
        if (!source->parameters[i].name ||
            !source->parameters[i].resolved_symbol_id ||
            source->parameters[i].value_type != MILENA_HIR_NUMBER ||
            typed->signature.parameter_types[i] != MILENA_IR_TYPE_F64)
            return false;
    }
    return true;
}

static bool validate_aot_function(const AOTContext *context,
                                  const AOTFunction *function,
                                  char *diagnostic, size_t diagnostic_capacity) {
    const MilenaIRProgram *ir = function ? function->ir : NULL;
    if (!ir || !ir->has_function_signature ||
        ir->signature.return_type != MILENA_IR_TYPE_F64 ||
        (ir->signature.parameter_count && !ir->signature.parameter_types)) {
        (void)snprintf(diagnostic, diagnostic_capacity,
                       "AOT admite únicamente firmas de función con retorno F64");
        return false;
    }
    for (size_t i = 0; i < ir->signature.parameter_count; ++i) {
        if (ir->signature.parameter_types[i] != MILENA_IR_TYPE_F64) {
            (void)snprintf(diagnostic, diagnostic_capacity,
                           "AOT admite únicamente parámetros F64");
            return false;
        }
    }
    for (size_t i = 0; i < ir->parameter_count; ++i) {
        if (!type_supported_value(ir->parameters[i].type)) {
            (void)snprintf(diagnostic, diagnostic_capacity,
                           "AOT no admite parámetros de bloque de tipo %d",
                           (int)ir->parameters[i].type);
            return false;
        }
    }
    for (size_t i = 0; i < ir->count; ++i) {
        const MilenaIRInstruction *ins = &ir->instructions[i];
        switch (ins->opcode) {
            case MILENA_IR_CONST_F64:
                if (ins->result_type != MILENA_IR_TYPE_F64 ||
                    !isfinite(ins->float_immediate)) goto unsupported_opcode;
                break;
            case MILENA_IR_CONST_BOOL:
                if (ins->result_type != MILENA_IR_TYPE_BOOL) goto unsupported_opcode;
                break;
            case MILENA_IR_ADD_F64:
            case MILENA_IR_SUB_F64:
            case MILENA_IR_MUL_F64:
            case MILENA_IR_DIV_F64:
                if (ins->result_type != MILENA_IR_TYPE_F64) goto unsupported_opcode;
                break;
            case MILENA_IR_EQ_F64:
            case MILENA_IR_NE_F64:
            case MILENA_IR_LT_F64:
            case MILENA_IR_LE_F64:
            case MILENA_IR_GT_F64:
            case MILENA_IR_GE_F64:
                if (ins->result_type != MILENA_IR_TYPE_BOOL) goto unsupported_opcode;
                break;
            case MILENA_IR_CALL: {
                uint32_t target = (uint32_t)ins->integer_immediate;
                const AOTFunction *callee = context_find_function(context, target);
                if (!callee || ins->integer_immediate <= 0 ||
                    ins->result_type != MILENA_IR_TYPE_F64 ||
                    ins->call_argument_count !=
                        callee->ir->signature.parameter_count) {
                    (void)snprintf(diagnostic, diagnostic_capacity,
                                   "AOT rechaza una llamada con target, aridad o retorno incompatible");
                    return false;
                }
                for (size_t ai = 0; ai < ins->call_argument_count; ++ai) {
                    if (callee->ir->signature.parameter_types[ai] !=
                        MILENA_IR_TYPE_F64) {
                        (void)snprintf(diagnostic, diagnostic_capacity,
                                       "AOT admite llamadas únicamente con parámetros F64");
                        return false;
                    }
                }
                break;
            }
            case MILENA_IR_BRANCH:
            case MILENA_IR_COND_BRANCH:
                break;
            case MILENA_IR_RETURN:
                if (ins->result_type != MILENA_IR_TYPE_F64 ||
                    ins->operand1_id == 0) goto unsupported_opcode;
                break;
            default:
                goto unsupported_opcode;
        }
    }
    return true;

unsupported_opcode:
    (void)snprintf(diagnostic, diagnostic_capacity,
                   "El opcode o tipo del IR tipado no pertenece al subconjunto AOT");
    return false;
}

static MilenaStatus prepare_typed_context(MilenaCanonicalProgram *program,
                                          AOTContext *context,
                                          MilenaError *error) {
    if (!program || !program->hir || !program->hir->functions ||
        !program->hir->function_count || program->hir->statement_count != 0) {
        return aot_unsupported(error,
            "AOT requiere funciones escalares y no admite sentencias globales");
    }
    context->module = program->typed_module;
    if (context->module) {
        char validation_error[MILENA_ERROR_TEXT] = {0};
        if (!milena_ir_module_validate(context->module, validation_error,
                                       sizeof(validation_error))) {
            aot_error(error, MILENA_ERR_UNSUPPORTED,
                      validation_error[0] ? validation_error :
                      "El módulo IR tipado no pasó su verificador");
            return MILENA_ERR_UNSUPPORTED;
        }
        context->function_count = context->module->function_count;
        if (context->function_count != program->hir->function_count)
            return aot_unsupported(error,
                "La identidad del módulo tipado no coincide con la HIR canónica");
        context->functions = calloc(context->function_count,
                                     sizeof(*context->functions));
        if (!context->functions) {
            aot_error(error, MILENA_ERR_MEMORY,
                      "Sin memoria para preparar las funciones AOT");
            return MILENA_ERR_MEMORY;
        }
        for (size_t i = 0; i < context->function_count; ++i) {
            const MilenaIRModuleFunction *source = &context->module->functions[i];
            context->functions[i].name = source->name;
            context->functions[i].symbol_id = source->symbol_id;
            context->functions[i].ir = source->body;
        }
        for (size_t i = 0; i < program->hir->function_count; ++i) {
            const MilenaHIRFunction *source = &program->hir->functions[i];
            if (!source->name || source->resolved_symbol_id == 0 ||
                source->resolved_symbol_id > UINT32_MAX) {
                aot_context_release(context);
                return aot_unsupported(error,
                    "La identidad fuente no cabe en el símbolo IR estable");
            }
            const MilenaIRModuleFunction *typed = module_find_function(
                context->module, (uint32_t)source->resolved_symbol_id);
            if (!typed || strcmp(typed->name, source->name) != 0 ||
                !validate_hir_signature(source, typed->body)) {
                aot_context_release(context);
                return aot_unsupported(error,
                    "La identidad del módulo tipado no coincide con la HIR canónica");
            }
        }
        if (!program->typed_ir || context->module->functions[0].body !=
                                  program->typed_ir) {
            aot_context_release(context);
            return aot_unsupported(error,
                "La vista de IR del programa no coincide con el módulo tipado");
        }
    } else {
        if (program->hir->function_count != 1 || !program->typed_ir) {
            return aot_unsupported(error,
                "La compilación AOT requiere una función IR tipada o un módulo tipado");
        }
        context->function_count = 1;
        context->functions = calloc(1, sizeof(*context->functions));
        if (!context->functions) {
            aot_error(error, MILENA_ERR_MEMORY,
                      "Sin memoria para preparar la función AOT");
            return MILENA_ERR_MEMORY;
        }
        const MilenaHIRFunction *source = &program->hir->functions[0];
        if (!source->name || source->resolved_symbol_id == 0 ||
            source->resolved_symbol_id > UINT32_MAX) {
            aot_context_release(context);
            return aot_unsupported(error,
                "La identidad de función fuente no cabe en el símbolo IR estable");
        }
        if (!validate_hir_signature(source, program->typed_ir)) {
            aot_context_release(context);
            return aot_unsupported(error,
                "La firma HIR no coincide con la firma de entrada IR tipada");
        }
        context->functions[0].name = source->name;
        context->functions[0].symbol_id = (uint32_t)source->resolved_symbol_id;
        context->functions[0].ir = program->typed_ir;
    }

    size_t principal_count = 0;
    for (size_t i = 0; i < context->function_count; ++i) {
        AOTFunction *function = &context->functions[i];
        if (!function->name || !function->symbol_id || !function->ir) {
            aot_context_release(context);
            return aot_unsupported(error,
                "El módulo IR contiene una función sin identidad completa");
        }
        for (size_t prior = 0; prior < i; ++prior) {
            if (context->functions[prior].symbol_id == function->symbol_id) {
                aot_context_release(context);
                return aot_unsupported(error,
                    "El módulo IR contiene símbolos de función duplicados");
            }
        }
        if (strcmp(function->name, "principal") == 0) {
            ++principal_count;
            context->entry_index = i;
        }
    }
    if (principal_count != 1 ||
        context->functions[context->entry_index].ir->signature.parameter_count != 0) {
        aot_context_release(context);
        return aot_unsupported(error,
            "AOT requiere exactamente una función principal() sin argumentos");
    }

    for (size_t i = 0; i < context->function_count; ++i) {
        char validation_error[MILENA_ERROR_TEXT] = {0};
        if (!milena_ir_program_validate(context->functions[i].ir,
                                        validation_error,
                                        sizeof(validation_error))) {
            aot_error(error, MILENA_ERR_UNSUPPORTED,
                      validation_error[0] ? validation_error :
                      "Una función IR tipada no pasó su verificador");
            aot_context_release(context);
            return MILENA_ERR_UNSUPPORTED;
        }
        if (!validate_aot_function(context, &context->functions[i],
                                   validation_error, sizeof(validation_error))) {
            aot_error(error, MILENA_ERR_UNSUPPORTED,
                      validation_error[0] ? validation_error :
                      "La función excede el subconjunto AOT");
            aot_context_release(context);
            return MILENA_ERR_UNSUPPORTED;
        }
    }
    return MILENA_OK;
}

static const char *c_type(MilenaIRType type) {
    switch (type) {
        case MILENA_IR_TYPE_F64: return "double";
        case MILENA_IR_TYPE_BOOL: return "int";
        default: return NULL;
    }
}

static bool emit_value(FILE *out, uint32_t function_id, uint32_t value_id) {
    return fprintf(out, "ml_v_f%" PRIu32 "_v%" PRIu32,
                   function_id, value_id) >= 0;
}

static bool emit_block_label(FILE *out, uint32_t function_id,
                             uint32_t block_id) {
    return fprintf(out, "ml_b_f%" PRIu32 "_b%" PRIu32,
                   function_id, block_id) >= 0;
}

static const MilenaIRBlockParameter *find_block_parameter(
    const MilenaIRProgram *ir, uint32_t block_id, size_t parameter_index) {
    size_t current = 0;
    for (size_t i = 0; i < ir->parameter_count; ++i) {
        if (ir->parameters[i].block_id != block_id) continue;
        if (current == parameter_index) return &ir->parameters[i];
        ++current;
    }
    return NULL;
}

static const MilenaIREdgeArgument *find_edge_argument(
    const MilenaIRProgram *ir, uint32_t source_block_id,
    uint32_t target_block_id, size_t parameter_index) {
    for (size_t i = 0; i < ir->edge_argument_count; ++i) {
        const MilenaIREdgeArgument *argument = &ir->edge_arguments[i];
        if (argument->source_block_id == source_block_id &&
            argument->target_block_id == target_block_id &&
            argument->parameter_index == parameter_index)
            return argument;
    }
    return NULL;
}

static size_t block_parameter_count(const MilenaIRProgram *ir,
                                   uint32_t block_id) {
    size_t count = 0;
    for (size_t i = 0; i < ir->parameter_count; ++i)
        if (ir->parameters[i].block_id == block_id) ++count;
    return count;
}

static bool emit_indent(FILE *out, unsigned int indent) {
    for (unsigned int i = 0; i < indent; ++i)
        if (fputs("    ", out) < 0) return false;
    return true;
}

static bool emit_edge_copy_name(FILE *out, uint32_t function_id,
                                uint32_t source_id, uint32_t target_id,
                                size_t parameter_index) {
    return fprintf(out,
                   "ml_copy_f%" PRIu32 "_b%" PRIu32 "_b%" PRIu32 "_p%zu",
                   function_id, source_id, target_id, parameter_index) >= 0;
}

/* Edge parameters behave like phi nodes. Capture every source first, then
 * assign destinations, so swaps/cycles are true parallel copies. */
static bool emit_edge_transition(FILE *out, const AOTFunction *function,
                                 uint32_t source_id, uint32_t target_id,
                                 unsigned int indent) {
    const MilenaIRProgram *ir = function->ir;
    size_t count = block_parameter_count(ir, target_id);
    for (size_t i = 0; i < count; ++i) {
        const MilenaIRBlockParameter *parameter =
            find_block_parameter(ir, target_id, i);
        const MilenaIREdgeArgument *argument =
            find_edge_argument(ir, source_id, target_id, i);
        if (!parameter || !argument || !emit_indent(out, indent) ||
            !emit_edge_copy_name(out, function->symbol_id, source_id,
                                 target_id, i) || fputs(" = ", out) < 0 ||
            !emit_value(out, function->symbol_id, argument->value_id) ||
            fputs(";\n", out) < 0) return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const MilenaIRBlockParameter *parameter =
            find_block_parameter(ir, target_id, i);
        if (!parameter || !emit_indent(out, indent) ||
            !emit_value(out, function->symbol_id, parameter->value_id) ||
            fputs(" = ", out) < 0 ||
            !emit_edge_copy_name(out, function->symbol_id, source_id,
                                 target_id, i) ||
            fputs(";\n", out) < 0) return false;
    }
    return emit_indent(out, indent) && fputs("goto ", out) >= 0 &&
           emit_block_label(out, function->symbol_id, target_id) &&
           fputs(";\n", out) >= 0;
}

static bool emit_call(FILE *out, const AOTContext *context,
                      const AOTFunction *function,
                      const MilenaIRInstruction *ins) {
    uint32_t target = (uint32_t)ins->integer_immediate;
    const AOTFunction *callee = context_find_function(context, target);
    if (!callee || fputs("ml_f_", out) < 0 ||
        fprintf(out, "f%" PRIu32 "(ml_depth + 1u", target) < 0) return false;
    for (size_t i = 0; i < ins->call_argument_count; ++i) {
        if (fputs(", ", out) < 0) return false;
        if (!emit_value(out, function->symbol_id,
                        function->ir->call_arguments[
                            ins->call_argument_offset + i])) return false;
    }
    return fputc(')', out) != EOF;
}

static bool emit_instruction(FILE *out, const AOTContext *context,
                             const AOTFunction *function,
                             const MilenaIRInstruction *ins) {
    if (!emit_indent(out, 1)) return false;
    switch (ins->opcode) {
        case MILENA_IR_CONST_F64:
            return emit_value(out, function->symbol_id, ins->result_id) &&
                   fprintf(out, " = %a;\n", ins->float_immediate) >= 0;
        case MILENA_IR_CONST_BOOL:
            return emit_value(out, function->symbol_id, ins->result_id) &&
                   fprintf(out, " = %s;\n", ins->integer_immediate ? "1" : "0") >= 0;
        case MILENA_IR_ADD_F64:
        case MILENA_IR_SUB_F64:
        case MILENA_IR_MUL_F64:
        case MILENA_IR_DIV_F64: {
            const char *helper = ins->opcode == MILENA_IR_ADD_F64 ? "ml_add" :
                ins->opcode == MILENA_IR_SUB_F64 ? "ml_sub" :
                ins->opcode == MILENA_IR_MUL_F64 ? "ml_mul" : "ml_div";
            return emit_value(out, function->symbol_id, ins->result_id) &&
                   fprintf(out, " = %s(", helper) >= 0 &&
                   emit_value(out, function->symbol_id, ins->operand1_id) &&
                   fputs(", ", out) >= 0 &&
                   emit_value(out, function->symbol_id, ins->operand2_id) &&
                   fputs(");\n", out) >= 0;
        }
        case MILENA_IR_EQ_F64:
        case MILENA_IR_NE_F64:
        case MILENA_IR_LT_F64:
        case MILENA_IR_LE_F64:
        case MILENA_IR_GT_F64:
        case MILENA_IR_GE_F64: {
            const char *operator_text = ins->opcode == MILENA_IR_EQ_F64 ? "==" :
                ins->opcode == MILENA_IR_NE_F64 ? "!=" :
                ins->opcode == MILENA_IR_LT_F64 ? "<" :
                ins->opcode == MILENA_IR_LE_F64 ? "<=" :
                ins->opcode == MILENA_IR_GT_F64 ? ">" : ">=";
            return emit_value(out, function->symbol_id, ins->result_id) &&
                   fputs(" = (", out) >= 0 &&
                   emit_value(out, function->symbol_id, ins->operand1_id) &&
                   fprintf(out, " %s ", operator_text) >= 0 &&
                   emit_value(out, function->symbol_id, ins->operand2_id) &&
                   fputs(");\n", out) >= 0;
        }
        case MILENA_IR_CALL:
            return emit_value(out, function->symbol_id, ins->result_id) &&
                   fputs(" = ", out) >= 0 &&
                   emit_call(out, context, function, ins) &&
                   fputs(";\n", out) >= 0;
        case MILENA_IR_BRANCH:
            return emit_edge_transition(out, function, ins->block_id,
                                        ins->target_true, 1);
        case MILENA_IR_COND_BRANCH:
            if (fputs("if (", out) < 0 ||
                !emit_value(out, function->symbol_id, ins->operand1_id) ||
                fputs(") {\n", out) < 0 ||
                !emit_edge_transition(out, function, ins->block_id,
                                      ins->target_true, 2) ||
                fputs("    } else {\n", out) < 0 ||
                !emit_edge_transition(out, function, ins->block_id,
                                      ins->target_false, 2) ||
                fputs("    }\n", out) < 0) return false;
            return true;
        case MILENA_IR_RETURN:
            return fputs("return ml_return(", out) >= 0 &&
                   emit_value(out, function->symbol_id, ins->operand1_id) &&
                   fputs(");\n", out) >= 0;
        default:
            return false;
    }
}

static bool emit_function(FILE *out, const AOTContext *context,
                          const AOTFunction *function) {
    const MilenaIRProgram *ir = function->ir;
    if (fprintf(out, "static double ml_f_f%" PRIu32 "(unsigned int ml_depth",
                function->symbol_id) < 0) return false;
    for (size_t i = 0; i < ir->signature.parameter_count; ++i) {
        if (fprintf(out, ", double ml_arg_f%" PRIu32 "_%zu",
                    function->symbol_id, i) < 0) return false;
    }
    if (fputs(") {\n"
              "    if (ml_depth >= 1000u) ml_fail(\"function call depth limit exceeded\");\n",
              out) < 0) return false;

    for (size_t i = 0; i < ir->parameter_count; ++i) {
        const MilenaIRBlockParameter *parameter = &ir->parameters[i];
        const char *type = c_type(parameter->type);
        if (!type || fprintf(out, "    %s ", type) < 0 ||
            !emit_value(out, function->symbol_id, parameter->value_id) ||
            fputs(";\n", out) < 0) return false;
    }
    for (size_t i = 0; i < ir->count; ++i) {
        const MilenaIRInstruction *ins = &ir->instructions[i];
        if (ins->opcode == MILENA_IR_BRANCH ||
            ins->opcode == MILENA_IR_COND_BRANCH ||
            ins->opcode == MILENA_IR_RETURN) continue;
        const char *type = c_type(ins->result_type);
        if (!type || fprintf(out, "    %s ", type) < 0 ||
            !emit_value(out, function->symbol_id, ins->result_id) ||
            fputs(";\n", out) < 0) return false;
    }
    for (size_t i = 0; i < ir->edge_argument_count; ++i) {
        const MilenaIREdgeArgument *argument = &ir->edge_arguments[i];
        const MilenaIRBlockParameter *parameter = find_block_parameter(
            ir, argument->target_block_id, argument->parameter_index);
        const char *type = parameter ? c_type(parameter->type) : NULL;
        if (!type || fprintf(out, "    %s ", type) < 0 ||
            !emit_edge_copy_name(out, function->symbol_id,
                                 argument->source_block_id,
                                 argument->target_block_id,
                                 argument->parameter_index) ||
            fputs(";\n", out) < 0) return false;
    }

    size_t entry_parameter = 0;
    if (!ir->block_count) return false;
    for (size_t i = 0; i < ir->parameter_count; ++i) {
        const MilenaIRBlockParameter *parameter = &ir->parameters[i];
        if (parameter->block_id != ir->blocks[0].id) continue;
        if (entry_parameter >= ir->signature.parameter_count ||
            fprintf(out, "    ") < 0 ||
            !emit_value(out, function->symbol_id, parameter->value_id) ||
            fprintf(out, " = ml_arg_f%" PRIu32 "_%zu;\n",
                    function->symbol_id, entry_parameter) < 0) return false;
        ++entry_parameter;
    }
    if (entry_parameter != ir->signature.parameter_count) return false;
    if (fprintf(out, "    goto ml_b_f%" PRIu32 "_b%" PRIu32 ";\n",
                function->symbol_id, ir->blocks[0].id) < 0) return false;

    for (size_t bi = 0; bi < ir->block_count; ++bi) {
        const MilenaIRBasicBlock *block = &ir->blocks[bi];
        if (!emit_block_label(out, function->symbol_id, block->id) ||
            fputs(": ;\n", out) < 0) return false;
        size_t end = block->first_instruction + block->instruction_count;
        for (size_t ii = block->first_instruction; ii < end; ++ii)
            if (!emit_instruction(out, context, function,
                                  &ir->instructions[ii])) return false;
    }
    return fputs("}\n\n", out) >= 0;
}

static bool emit_program(FILE *out, const AOTContext *context) {
    if (fputs("#include <math.h>\n#include <stdio.h>\n#include <stdlib.h>\n\n"
              "static void ml_fail(const char *message) {\n"
              "    (void)fprintf(stderr, \"Milena native runtime error: %s\\n\", message);\n"
              "    exit(70);\n"
              "}\n"
              "static double ml_check(double value) {\n"
              "    if (!isfinite(value)) ml_fail(\"non-finite numeric result\");\n"
              "    return value;\n"
              "}\n"
              "static double ml_return(double value) { return ml_check(value); }\n"
              "static double ml_add(double a, double b) { return ml_check(a + b); }\n"
              "static double ml_sub(double a, double b) { return ml_check(a - b); }\n"
              "static double ml_mul(double a, double b) { return ml_check(a * b); }\n"
              "static double ml_div(double a, double b) {\n"
              "    if (b == 0.0) ml_fail(\"division by zero\");\n"
              "    return ml_check(a / b);\n"
              "}\n\n", out) < 0) return false;

    for (size_t i = 0; i < context->function_count; ++i) {
        const AOTFunction *function = &context->functions[i];
        if (fprintf(out, "static double ml_f_f%" PRIu32 "(unsigned int",
                    function->symbol_id) < 0) return false;
        for (size_t p = 0; p < function->ir->signature.parameter_count; ++p) {
            if (fputs(", double", out) < 0) return false;
        }
        if (fputs(");\n", out) < 0) return false;
    }
    if (fputc('\n', out) == EOF) return false;
    for (size_t i = 0; i < context->function_count; ++i)
        if (!emit_function(out, context, &context->functions[i])) return false;
    const AOTFunction *entry = &context->functions[context->entry_index];
    return fprintf(out,
                   "int main(void) {\n"
                   "    double result = ml_f_f%" PRIu32 "(0u);\n"
                   "    if (printf(\"%%.17g\\n\", result) < 0 || ferror(stdout)) return 74;\n"
                   "    return 0;\n"
                   "}\n", entry->symbol_id) >= 0;
}

#if !defined(_WIN32)
static char *temporary_template(const char *output_filename,
                                const char *prefix) {
    const char *slash = strrchr(output_filename, '/');
    size_t directory_length = 0;
    bool root_directory = slash == output_filename;
    if (slash && !root_directory)
        directory_length = (size_t)(slash - output_filename);
    else if (root_directory)
        directory_length = 1;
    const char *leading = output_filename[0] == '/' ? "" : "./";
    const char *separator = directory_length && !root_directory ? "/" : "";
    size_t total = strlen(leading);
    if (!milena_size_add(total, directory_length, &total) ||
        !milena_size_add(total, strlen(separator), &total) ||
        !milena_size_add(total, strlen(prefix), &total) ||
        !milena_size_add(total, sizeof("XXXXXX"), &total)) return NULL;
    char *path = malloc(total);
    if (!path) return NULL;
    size_t offset = 0;
    size_t length = strlen(leading);
    memcpy(path + offset, leading, length);
    offset += length;
    if (directory_length) {
        memcpy(path + offset, output_filename, directory_length);
        offset += directory_length;
    }
    length = strlen(separator);
    memcpy(path + offset, separator, length);
    offset += length;
    length = strlen(prefix);
    memcpy(path + offset, prefix, length);
    offset += length;
    memcpy(path + offset, "XXXXXX", sizeof("XXXXXX"));
    return path;
}

static bool write_generated_source(const AOTContext *context,
                                  const char *filename) {
    int fd = mkstemp((char *)filename);
    if (fd < 0) return false;
    FILE *out = fdopen(fd, "w");
    if (!out) {
        (void)close(fd);
        (void)unlink(filename);
        return false;
    }
    bool ok = emit_program(out, context) && !ferror(out);
    if (fclose(out) != 0) ok = false;
    if (!ok) (void)unlink(filename);
    return ok;
}

static int run_c_compiler(const char *compiler, const char *source_path,
                          const char *binary_path) {
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        char *const arguments[] = {
            (char *)compiler, (char *)"-std=c17", (char *)"-O2",
            (char *)"-fno-fast-math", (char *)"-x", (char *)"c",
            (char *)source_path, (char *)"-lm", (char *)"-o",
            (char *)binary_path, NULL
        };
        execvp(compiler, arguments);
        static const char message[] =
            "Milena: could not execute the configured host C compiler\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(127);
    }
    int status;
    for (;;) {
        pid_t result = waitpid(child, &status, 0);
        if (result == child) break;
        if (result < 0 && errno == EINTR) continue;
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static bool source_output_alias(const char *source_filename,
                                const char *output_filename) {
    struct stat source_stat;
    struct stat output_stat;
    if (stat(source_filename, &source_stat) != 0) return false;
    if (stat(output_filename, &output_stat) != 0) return false;
    return source_stat.st_dev == output_stat.st_dev &&
           source_stat.st_ino == output_stat.st_ino;
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

static MilenaStatus build_from_typed_ir(MilenaCanonicalProgram *program,
                                        const char *output_filename,
                                        MilenaError *error) {
    AOTContext context = {0};
    MilenaStatus status = prepare_typed_context(program, &context, error);
    if (status != MILENA_OK) return status;
#if defined(_WIN32)
    (void)output_filename;
    aot_context_release(&context);
    return aot_unsupported(error,
        "La compilación AOT nativa requiere un host POSIX; Windows no está habilitado");
#else
    char *source_template = temporary_template(output_filename,
                                                ".milena-aot-src-");
    char *binary_template = temporary_template(output_filename,
                                                ".milena-aot-bin-");
    if (!source_template || !binary_template) {
        free(source_template);
        free(binary_template);
        aot_context_release(&context);
        aot_error(error, MILENA_ERR_MEMORY,
                  "Sin memoria para preparar archivos temporales AOT");
        return MILENA_ERR_MEMORY;
    }
    int binary_fd = mkstemp(binary_template);
    if (binary_fd < 0) {
        free(source_template);
        free(binary_template);
        aot_context_release(&context);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo crear el archivo temporal de salida AOT");
        return MILENA_ERR_IO;
    }
    if (close(binary_fd) != 0) {
        (void)unlink(binary_template);
        free(source_template);
        free(binary_template);
        aot_context_release(&context);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo cerrar el archivo temporal de salida AOT");
        return MILENA_ERR_IO;
    }
    if (!write_generated_source(&context, source_template)) {
        (void)unlink(binary_template);
        (void)unlink(source_template);
        free(source_template);
        free(binary_template);
        aot_context_release(&context);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo escribir el C generado desde el IR tipado");
        return MILENA_ERR_IO;
    }
    const char *compiler = getenv("MILENA_CC");
    if (!compiler || !compiler[0]) compiler = "cc";
    int compile_status = run_c_compiler(compiler, source_template,
                                        binary_template);
    (void)unlink(source_template);
    free(source_template);
    if (compile_status != 0) {
        (void)unlink(binary_template);
        free(binary_template);
        aot_context_release(&context);
        aot_error(error, MILENA_ERR_INTERNAL,
                  "El compilador C no produjo el ejecutable AOT; se conservó la salida previa");
        return MILENA_ERR_INTERNAL;
    }
    struct stat candidate_stat;
    if (lstat(binary_template, &candidate_stat) != 0 ||
        !S_ISREG(candidate_stat.st_mode) || candidate_stat.st_size <= 0) {
        (void)unlink(binary_template);
        free(binary_template);
        aot_context_release(&context);
        aot_error(error, MILENA_ERR_INTERNAL,
                  "El compilador C no dejó un ejecutable AOT válido");
        return MILENA_ERR_INTERNAL;
    }
    if (chmod(binary_template, 0755) != 0 ||
        rename(binary_template, output_filename) != 0) {
        (void)unlink(binary_template);
        free(binary_template);
        aot_context_release(&context);
        aot_error(error, MILENA_ERR_IO,
                  "No se pudo instalar atómicamente el ejecutable AOT solicitado");
        return MILENA_ERR_IO;
    }
    free(binary_template);
    aot_context_release(&context);
    return MILENA_OK;
#endif
}
#endif /* !defined(_WIN32) */

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
#if defined(_WIN32)
    return aot_unsupported(error,
        "La compilación AOT nativa requiere un host POSIX; Windows no está habilitado");
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
