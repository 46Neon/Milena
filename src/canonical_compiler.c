#include "canonical_compiler.h"

#include "language_semantic.h"
#include "lexer.h"
#include "parser.h"

static void canonical_error(MilenaError *error, MilenaStatus code,
                            const char *message) {
    if (error) milena_error_set(error, code, 0, 0, 0, message);
}

void milena_canonical_program_init(MilenaCanonicalProgram *program) {
    if (!program) return;
    program->ast = NULL;
    program->table = NULL;
}

void milena_canonical_program_release(MilenaCanonicalProgram *program) {
    if (!program) return;
    ast_destroy(program->ast);
    program->ast = NULL;
    program->table = NULL;
}

MilenaStatus milena_canonical_program_parse(MilenaCanonicalProgram *program,
                                             const char *source,
                                             MilenaError *error) {
    if (!program || !source) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "El programa canónico y su fuente son obligatorios");
        return MILENA_ERR_ARGUMENT;
    }
    milena_canonical_program_release(program);
    if (error) milena_error_clear(error);

    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    ASTNode *ast = parser_parse(&parser);
    if (!ast || parser.has_error) {
        if (error) {
            if (parser.error.code != MILENA_OK) *error = parser.error;
            else canonical_error(error, MILENA_ERR_PARSE,
                                 "La fuente no produjo un AST canónico válido");
        }
        ast_destroy(ast);
        parser_release(&parser);
        return error && error->code != MILENA_OK ? error->code : MILENA_ERR_PARSE;
    }
    parser_release(&parser);

    MilenaStatus status = milena_validate_ast(ast, error);
    if (status != MILENA_OK) {
        ast_destroy(ast);
        return status;
    }
    program->ast = ast;
    return MILENA_OK;
}

MilenaStatus milena_canonical_program_bind_table(MilenaCanonicalProgram *program,
                                                 const MilenaTable *table,
                                                 MilenaError *error) {
    if (!program || !program->ast || !table) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "La frontera canónica requiere AST y MilenaTable");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_table_validate(table, error);
    if (status != MILENA_OK) return status;

    const ASTNode *analysis = NULL;
    for (size_t i = 0; i < program->ast->child_count; i++) {
        const ASTNode *child = program->ast->children[i];
        if (child && child->type == AST_BLOQUE_ANALISIS) {
            analysis = child;
            break;
        }
    }
    if (analysis) {
        status = milena_validate_sst_table(analysis, table, error);
        if (status != MILENA_OK) return status;
    }
    program->table = table;
    return MILENA_OK;
}

MilenaStatus milena_canonical_compiler_input(
    const MilenaCanonicalProgram *program,
    MilenaCanonicalCompilerInput *input,
    MilenaError *error) {
    if (!program || !program->ast || !input) {
        canonical_error(error, MILENA_ERR_ARGUMENT,
                        "La entrada del compilador canónico es inválida");
        return MILENA_ERR_ARGUMENT;
    }
    input->ast = program->ast;
    input->table = program->table;
    return MILENA_OK;
}
