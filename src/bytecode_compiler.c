#include "bytecode_compiler.h"

#include "canonical_compiler.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t symbol_id;
    uint32_t reg;
    MilenaHIRValueType type;
} LocalRegister;

typedef struct {
    MilenaBytecodeInstruction *instructions;
    size_t instruction_count;
    size_t instruction_capacity;
    LocalRegister *locals;
    size_t local_count;
    size_t local_capacity;
    uint16_t register_count;
    MilenaError *error;
} Lowering;

#define LOWERING_MAX_DEPTH 256u

static MilenaStatus fail_at(MilenaError *error, MilenaStatus status,
                            const MilenaHIRSourceSpan *span,
                            const char *message) {
    if (error) {
        milena_error_set(error, status,
                         span && span->has_source_span ? span->line : 0,
                         span && span->has_source_span ? span->column : 0,
                         0, message);
    }
    return status;
}

static bool reject_at(Lowering *lowering,
                      const MilenaHIRSourceSpan *span,
                      const char *message) {
    (void)fail_at(lowering->error, MILENA_ERR_UNSUPPORTED, span, message);
    return false;
}

static bool memory_error(Lowering *lowering,
                         const MilenaHIRSourceSpan *span,
                         const char *message) {
    (void)fail_at(lowering->error, MILENA_ERR_MEMORY, span, message);
    return false;
}

static bool ensure_instruction_capacity(Lowering *lowering,
                                        const MilenaHIRSourceSpan *span) {
    if (lowering->instruction_count >= MILENA_BYTECODE_MAX_INSTRUCTIONS) {
        return reject_at(lowering, span,
                         "Bytecode v1 excede el máximo de instrucciones");
    }
    if (lowering->instruction_count < lowering->instruction_capacity) return true;
    size_t next = lowering->instruction_capacity ?
        lowering->instruction_capacity * 2u : 32u;
    if (next > MILENA_BYTECODE_MAX_INSTRUCTIONS)
        next = MILENA_BYTECODE_MAX_INSTRUCTIONS;
    if (next <= lowering->instruction_count ||
        next > SIZE_MAX / sizeof(*lowering->instructions)) {
        return reject_at(lowering, span,
                         "Bytecode v1 excede el límite de almacenamiento");
    }
    MilenaBytecodeInstruction *grown = realloc(
        lowering->instructions, next * sizeof(*grown));
    if (!grown)
        return memory_error(lowering, span,
                            "Sin memoria para emitir bytecode v1");
    lowering->instructions = grown;
    lowering->instruction_capacity = next;
    return true;
}

static bool emit(Lowering *lowering, MilenaBytecodeOpcode opcode,
                 uint32_t a, uint32_t b, uint32_t c, double immediate,
                 const MilenaHIRSourceSpan *span, size_t *index_out) {
    if (!ensure_instruction_capacity(lowering, span)) return false;
    if (index_out) *index_out = lowering->instruction_count;
    lowering->instructions[lowering->instruction_count++] =
        (MilenaBytecodeInstruction){(uint8_t)opcode, a, b, c, immediate};
    return true;
}

static bool patch_target(Lowering *lowering, size_t instruction_index,
                         size_t target, const MilenaHIRSourceSpan *span) {
    if (instruction_index >= lowering->instruction_count ||
        target >= MILENA_BYTECODE_MAX_INSTRUCTIONS || target > UINT32_MAX) {
        return reject_at(lowering, span,
                         "Destino de rama fuera de los límites de bytecode v1");
    }
    MilenaBytecodeInstruction *instruction =
        &lowering->instructions[instruction_index];
    if (instruction->opcode == MILENA_BC_JUMP)
        instruction->a = (uint32_t)target;
    else if (instruction->opcode == MILENA_BC_JUMP_IF_FALSE)
        instruction->b = (uint32_t)target;
    else
        return reject_at(lowering, span,
                         "Invariante interna de parcheo de rama inválida");
    return true;
}

static bool allocate_register(Lowering *lowering,
                              const MilenaHIRSourceSpan *span,
                              uint32_t *reg_out) {
    if (lowering->register_count >= MILENA_BYTECODE_MAX_REGISTERS) {
        return reject_at(lowering, span,
                         "Bytecode v1 excede el máximo de registros");
    }
    *reg_out = (uint32_t)lowering->register_count;
    lowering->register_count = (uint16_t)(lowering->register_count + 1u);
    return true;
}

static LocalRegister *find_local(Lowering *lowering, size_t symbol_id) {
    if (symbol_id == 0) return NULL;
    for (size_t i = lowering->local_count; i > 0; --i) {
        if (lowering->locals[i - 1].symbol_id == symbol_id)
            return &lowering->locals[i - 1];
    }
    return NULL;
}

static bool bind_local(Lowering *lowering, size_t symbol_id, uint32_t reg,
                       MilenaHIRValueType type,
                       const MilenaHIRSourceSpan *span) {
    if (!symbol_id || find_local(lowering, symbol_id))
        return reject_at(lowering, span,
                         "Binding local ausente o duplicado en HIR escalar");
    if (lowering->local_count >= MILENA_BYTECODE_MAX_REGISTERS)
        return reject_at(lowering, span,
                         "Bytecode v1 excede el máximo de variables locales");
    if (lowering->local_count == lowering->local_capacity) {
        size_t next = lowering->local_capacity ?
            lowering->local_capacity * 2u : 16u;
        if (next > MILENA_BYTECODE_MAX_REGISTERS)
            next = MILENA_BYTECODE_MAX_REGISTERS;
        if (next <= lowering->local_count ||
            next > SIZE_MAX / sizeof(*lowering->locals))
            return reject_at(lowering, span,
                             "Tabla local fuera de los límites de bytecode v1");
        LocalRegister *grown = realloc(lowering->locals, next * sizeof(*grown));
        if (!grown)
            return memory_error(lowering, span,
                                "Sin memoria para bindings locales de bytecode");
        lowering->locals = grown;
        lowering->local_capacity = next;
    }
    lowering->locals[lowering->local_count++] =
        (LocalRegister){symbol_id, reg, type};
    return true;
}

static bool scalar_type(MilenaHIRValueType type) {
    return type == MILENA_HIR_NUMBER || type == MILENA_HIR_BOOLEAN;
}

static bool lower_expression(Lowering *lowering,
                             const MilenaHIRExpression *expression,
                             unsigned depth, uint32_t *reg_out) {
    if (!expression || !reg_out)
        return reject_at(lowering, expression ? &expression->span : NULL,
                         "Expresión HIR nula o incompleta");
    if (depth > LOWERING_MAX_DEPTH)
        return reject_at(lowering, &expression->span,
                         "Expresión HIR demasiado profunda para bytecode v1");
    if (!scalar_type(expression->value_type))
        return reject_at(lowering, &expression->span,
                         "Tipo escalar HIR no admitido por bytecode v1");

    switch (expression->kind) {
        case MILENA_HIR_EXPR_LITERAL: {
            double value;
            if (expression->value_type == MILENA_HIR_NUMBER) {
                value = expression->as.number;
                if (!isfinite(value))
                    return reject_at(lowering, &expression->span,
                                     "Literal numérico no finito no admitido");
            } else {
                value = expression->as.boolean ? 1.0 : 0.0;
            }
            if (!allocate_register(lowering, &expression->span, reg_out))
                return false;
            return emit(lowering, MILENA_BC_CONST_F64, *reg_out, 0, 0,
                        value, &expression->span, NULL);
        }
        case MILENA_HIR_EXPR_VARIABLE: {
            LocalRegister *local = find_local(
                lowering, expression->resolved_symbol_id);
            if (!local)
                return reject_at(lowering, &expression->span,
                                 "Referencia a local sin binding HIR disponible");
            if (local->type != expression->value_type)
                return reject_at(lowering, &expression->span,
                                 "Tipo de referencia local no coincide con HIR");
            *reg_out = local->reg;
            return true;
        }
        case MILENA_HIR_EXPR_BINARY: {
            uint32_t left, right;
            if (!expression->as.binary.left || !expression->as.binary.right)
                return reject_at(lowering, &expression->span,
                                 "Operación HIR sin ambos operandos");
            const MilenaHIRExpression *lhs = expression->as.binary.left;
            const MilenaHIRExpression *rhs = expression->as.binary.right;
            ASTOperatorKind operation = expression->as.binary.operation;
            MilenaBytecodeOpcode opcode = MILENA_BC_ADD;
            bool comparison = true;
            switch (operation) {
                case AST_OPERATOR_ADD: opcode = MILENA_BC_ADD; comparison = false; break;
                case AST_OPERATOR_SUBTRACT: opcode = MILENA_BC_SUB; comparison = false; break;
                case AST_OPERATOR_MULTIPLY: opcode = MILENA_BC_MUL; comparison = false; break;
                case AST_OPERATOR_DIVIDE: opcode = MILENA_BC_DIV; comparison = false; break;
                case AST_OPERATOR_EQUAL: opcode = MILENA_BC_EQ; break;
                case AST_OPERATOR_NOT_EQUAL: opcode = MILENA_BC_NE; break;
                case AST_OPERATOR_LESS: opcode = MILENA_BC_LT; break;
                case AST_OPERATOR_LESS_EQUAL: opcode = MILENA_BC_LE; break;
                case AST_OPERATOR_GREATER: opcode = MILENA_BC_GT; break;
                case AST_OPERATOR_GREATER_EQUAL: opcode = MILENA_BC_GE; break;
                default:
                    return reject_at(lowering, &expression->span,
                                     "Operador HIR no representado en bytecode v1");
            }
            if (comparison) {
                if (lhs->value_type != rhs->value_type ||
                    (lhs->value_type == MILENA_HIR_BOOLEAN &&
                     operation != AST_OPERATOR_EQUAL &&
                     operation != AST_OPERATOR_NOT_EQUAL))
                    return reject_at(lowering, &expression->span,
                                     "Comparación HIR mixta o booleana relacional no admitida");
                if (expression->value_type != MILENA_HIR_BOOLEAN)
                    return reject_at(lowering, &expression->span,
                                     "Comparación HIR sin resultado booleano");
            } else if (lhs->value_type != MILENA_HIR_NUMBER ||
                       rhs->value_type != MILENA_HIR_NUMBER ||
                       expression->value_type != MILENA_HIR_NUMBER) {
                return reject_at(lowering, &expression->span,
                                 "La aritmética de bytecode v1 solo admite números");
            }
            if (!lower_expression(lowering, lhs, depth + 1u, &left) ||
                !lower_expression(lowering, rhs, depth + 1u, &right) ||
                !allocate_register(lowering, &expression->span, reg_out))
                return false;
            return emit(lowering, opcode, *reg_out, left, right, 0.0,
                        &expression->span, NULL);
        }
        case MILENA_HIR_EXPR_CALL:
            return reject_at(lowering, &expression->span,
                             "Las llamadas de función y sus argumentos no están admitidos en bytecode v1");
        default:
            return reject_at(lowering, &expression->span,
                             "Variante de expresión HIR no admitida en bytecode v1");
    }
}

static bool lower_statement_list(Lowering *lowering,
                                 MilenaHIRStatement *const *statements,
                                 size_t statement_count, unsigned depth,
                                 bool *terminates);

static bool lower_statement(Lowering *lowering,
                            const MilenaHIRStatement *statement,
                            unsigned depth, bool *terminates) {
    if (!statement || !terminates)
        return reject_at(lowering, statement ? &statement->span : NULL,
                         "Sentencia HIR nula o incompleta");
    if (depth > LOWERING_MAX_DEPTH)
        return reject_at(lowering, &statement->span,
                         "Anidamiento HIR demasiado profundo para bytecode v1");
    *terminates = false;
    switch (statement->kind) {
        case MILENA_HIR_STMT_DECLARE: {
            uint32_t destination, source;
            if (!scalar_type(statement->value_type) ||
                !statement->as.expression)
                return reject_at(lowering, &statement->span,
                                 "Declaración HIR requiere inicializador escalar tipado");
            if (!allocate_register(lowering, &statement->span, &destination) ||
                !bind_local(lowering, statement->resolved_symbol_id,
                            destination, statement->value_type, &statement->span) ||
                !lower_expression(lowering, statement->as.expression, 0, &source))
                return false;
            if (statement->as.expression->value_type != statement->value_type)
                return reject_at(lowering, &statement->span,
                                 "Tipo de inicializador distinto del binding local");
            return emit(lowering, MILENA_BC_MOVE, destination, source, 0, 0.0,
                        &statement->span, NULL);
        }
        case MILENA_HIR_STMT_ASSIGN: {
            LocalRegister *local = find_local(
                lowering, statement->resolved_symbol_id);
            uint32_t source;
            if (!local)
                return reject_at(lowering, &statement->span,
                                 "Asignación HIR sin binding local disponible");
            if (!statement->as.expression ||
                local->type != statement->value_type ||
                statement->as.expression->value_type != local->type)
                return reject_at(lowering, &statement->span,
                                 "Asignación HIR con tipo incompatible");
            if (!lower_expression(lowering, statement->as.expression, 0, &source))
                return false;
            return emit(lowering, MILENA_BC_MOVE, local->reg, source, 0, 0.0,
                        &statement->span, NULL);
        }
        case MILENA_HIR_STMT_RETURN: {
            uint32_t result;
            if (!statement->as.expression ||
                statement->value_type != MILENA_HIR_NUMBER ||
                statement->as.expression->value_type != MILENA_HIR_NUMBER)
                return reject_at(lowering, &statement->span,
                                 "Bytecode v1 solo admite retorno numérico");
            if (!lower_expression(lowering, statement->as.expression, 0, &result) ||
                !emit(lowering, MILENA_BC_RETURN, result, 0, 0, 0.0,
                      &statement->span, NULL))
                return false;
            *terminates = true;
            return true;
        }
        case MILENA_HIR_STMT_IF: {
            uint32_t condition;
            size_t false_branch;
            if (!statement->as.conditional.condition)
                return reject_at(lowering, &statement->span,
                                 "Condición HIR ausente");
            if (!lower_expression(lowering,
                                  statement->as.conditional.condition, 0,
                                  &condition) ||
                !emit(lowering, MILENA_BC_JUMP_IF_FALSE, condition,
                      UINT32_MAX, 0, 0.0, &statement->span, &false_branch))
                return false;
            bool then_terminates = false;
            if (!lower_statement_list(
                    lowering, statement->as.conditional.then_body,
                    statement->as.conditional.then_count, depth + 1u,
                    &then_terminates))
                return false;
            bool has_else = statement->as.conditional.else_count != 0;
            size_t end_branch = SIZE_MAX;
            if (has_else && !then_terminates &&
                !emit(lowering, MILENA_BC_JUMP, UINT32_MAX, 0, 0, 0.0,
                      &statement->span, &end_branch))
                return false;
            if (!patch_target(lowering, false_branch,
                              lowering->instruction_count, &statement->span))
                return false;
            bool else_terminates = false;
            if (has_else &&
                !lower_statement_list(
                    lowering, statement->as.conditional.else_body,
                    statement->as.conditional.else_count, depth + 1u,
                    &else_terminates))
                return false;
            if (end_branch != SIZE_MAX &&
                !patch_target(lowering, end_branch,
                              lowering->instruction_count, &statement->span))
                return false;
            *terminates = has_else && then_terminates && else_terminates;
            return true;
        }
        default:
            return reject_at(lowering, &statement->span,
                             "Variante de sentencia HIR no admitida en bytecode v1");
    }
}

static bool lower_statement_list(Lowering *lowering,
                                 MilenaHIRStatement *const *statements,
                                 size_t statement_count, unsigned depth,
                                 bool *terminates) {
    bool already_terminates = false;
    if (statement_count && !statements)
        return reject_at(lowering, NULL,
                         "Lista de sentencias HIR sin almacenamiento");
    for (size_t i = 0; i < statement_count; ++i) {
        if (already_terminates) {
            if (!statements[i])
                return reject_at(lowering, NULL,
                                 "Sentencia HIR nula después de retorno");
            return reject_at(lowering, &statements[i]->span,
                             "Sentencia inalcanzable después de retorno en bytecode v1");
        }
        bool statement_terminates = false;
        if (!lower_statement(lowering, statements[i], depth,
                             &statement_terminates))
            return false;
        already_terminates = statement_terminates;
    }
    *terminates = already_terminates;
    return true;
}

static bool validate_entry_function(const MilenaScalarHIR *hir,
                                    const MilenaHIRFunction **entry,
                                    MilenaError *error) {
    *entry = NULL;
    if (!hir) {
        (void)fail_at(error, MILENA_ERR_UNSUPPORTED, NULL,
                      "No existe HIR escalar canónica para lowering bytecode v1");
        return false;
    }
    if (hir->statement_count != 0) {
        (void)fail_at(error, MILENA_ERR_UNSUPPORTED,
                      hir->statement_count && hir->statements ?
                          &hir->statements[0]->span : NULL,
                      "El bytecode v1 solo admite una función principal, sin sentencias globales");
        return false;
    }
    if (hir->function_count != 1 || !hir->functions) {
        (void)fail_at(error, MILENA_ERR_UNSUPPORTED,
                      hir->function_count && hir->functions ?
                          &hir->functions[0].span : NULL,
                      "El bytecode v1 solo admite una función: principal");
        return false;
    }
    const MilenaHIRFunction *function = &hir->functions[0];
    if (!function->name || strcmp(function->name, "principal") != 0) {
        (void)fail_at(error, MILENA_ERR_UNSUPPORTED, &function->span,
                      "La única función bytecode v1 debe llamarse principal");
        return false;
    }
    if (function->parameter_count != 0) {
        (void)fail_at(error, MILENA_ERR_UNSUPPORTED, &function->span,
                      "principal debe tener cero parámetros en bytecode v1");
        return false;
    }
    *entry = function;
    return true;
}

MilenaStatus milena_bytecode_compile_source(const char *source,
                                             uint8_t **bytes_out,
                                             size_t *length_out,
                                             MilenaError *error) {
    MilenaError local_error;
    if (!error) error = &local_error;
    if (bytes_out) *bytes_out = NULL;
    if (length_out) *length_out = 0;
    milena_error_clear(error);
    if (!source || !bytes_out || !length_out) {
        milena_error_set(error, MILENA_ERR_ARGUMENT, 0, 0, 0,
                         "Fuente y salidas son obligatorias para compilar bytecode v1");
        return MILENA_ERR_ARGUMENT;
    }

    MilenaCanonicalProgram canonical;
    milena_canonical_program_init(&canonical);
    MilenaStatus status = milena_canonical_program_parse(
        &canonical, source, error);
    if (status != MILENA_OK) {
        milena_canonical_program_release(&canonical);
        return status;
    }
    MilenaCanonicalCompilerInput canonical_input = {0};
    status = milena_canonical_compiler_input(&canonical, &canonical_input, error);
    if (status != MILENA_OK) {
        milena_canonical_program_release(&canonical);
        return status;
    }

    const MilenaHIRFunction *entry = NULL;
    if (!validate_entry_function(canonical_input.hir, &entry, error)) {
        status = error->code != MILENA_OK ? error->code : MILENA_ERR_UNSUPPORTED;
        milena_canonical_program_release(&canonical);
        return status;
    }
    MilenaHIRSourceSpan entry_span = entry->span;

    Lowering lowering = {0};
    lowering.error = error;
    bool terminates = false;
    if (!lower_statement_list(&lowering, entry->body, entry->body_count, 0,
                              &terminates)) {
        status = error->code != MILENA_OK ? error->code : MILENA_ERR_UNSUPPORTED;
        free(lowering.instructions);
        free(lowering.locals);
        milena_canonical_program_release(&canonical);
        return status;
    }
    if (!terminates) {
        status = fail_at(error, MILENA_ERR_UNSUPPORTED, &entry_span,
                         "Todos los caminos de principal deben terminar en retorno numérico");
        free(lowering.instructions);
        free(lowering.locals);
        milena_canonical_program_release(&canonical);
        return status;
    }
    if (lowering.instruction_count == 0 || lowering.register_count == 0) {
        status = fail_at(error, MILENA_ERR_UNSUPPORTED, &entry_span,
                         "principal no produjo una función bytecode ejecutable");
        free(lowering.instructions);
        free(lowering.locals);
        milena_canonical_program_release(&canonical);
        return status;
    }

    MilenaBytecodeProgram program = {
        MILENA_BYTECODE_VERSION_MAJOR,
        MILENA_BYTECODE_VERSION_MINOR,
        lowering.register_count,
        lowering.instruction_count,
        lowering.instructions
    };
    size_t required = 0;
    MilenaBytecodeDiagnostic diagnostic;
    MilenaBytecodeStatus bytecode_status = milena_bytecode_encode(
        &program, NULL, 0, &required, NULL, &diagnostic);
    if (bytecode_status != MILENA_BC_BUFFER_TOO_SMALL) {
        char message[160];
        (void)snprintf(message, sizeof(message),
                       "No se pudo dimensionar bytecode v1: %s",
                       milena_bytecode_status_name(bytecode_status));
        status = fail_at(error, MILENA_ERR_INTERNAL, &entry_span, message);
        free(lowering.instructions);
        free(lowering.locals);
        milena_canonical_program_release(&canonical);
        return status;
    }
    uint8_t *encoded = malloc(required);
    if (!encoded) {
        status = fail_at(error, MILENA_ERR_MEMORY, &entry_span,
                         "Sin memoria para serializar bytecode v1");
        free(lowering.instructions);
        free(lowering.locals);
        milena_canonical_program_release(&canonical);
        return status;
    }
    bytecode_status = milena_bytecode_encode(
        &program, encoded, required, &required, NULL, &diagnostic);
    free(lowering.instructions);
    free(lowering.locals);
    milena_canonical_program_release(&canonical);
    if (bytecode_status != MILENA_BC_OK) {
        char message[160];
        (void)snprintf(message, sizeof(message),
                       "El bytecode generado no pasó encode/verify: %s",
                       milena_bytecode_status_name(bytecode_status));
        free(encoded);
        status = fail_at(error, MILENA_ERR_INTERNAL, &entry_span, message);
        return status;
    }
    *bytes_out = encoded;
    *length_out = required;
    if (error) milena_error_clear(error);
    return MILENA_OK;
}
