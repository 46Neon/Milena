#include "parser.h"

#include <assert.h>
#include <string.h>

int main(void) {
    const char *source =
        ". analisis demo {\n"
        "  variable base = 10;\n"
        "  variable total = base + 5;\n"
        "}\n";
    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    ASTNode *program = parser_parse(&parser);
    assert(program != NULL);
    assert(!parser.has_error);

    MilenaError error;
    milena_error_init(&error);
    assert(ast_validate(program, &error));
    assert(error.code == MILENA_OK);

    ASTNode *analysis = program->children[0];
    ASTNode *declaration = analysis->children[1];
    ASTNode *binary = declaration->children[0];
    assert(binary->type == AST_EXPRESION_OPERACION);
    assert(binary->child_count == 2);

    ASTNode *saved_parent = binary->parent;
    binary->parent = NULL;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    assert(error.line == (size_t)binary->line);
    binary->parent = saved_parent;
    assert(ast_validate(program, &error));

    ASTNodeType saved_type = binary->type;
    binary->type = (ASTNodeType)AST_NODE_TYPE_COUNT;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    binary->type = saved_type;
    assert(ast_validate(program, &error));

    ASTOperatorKind saved_operator = binary->operator_kind;
    binary->operator_kind = AST_OPERATOR_NONE;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    binary->operator_kind = saved_operator;
    assert(binary->left_operand == binary->children[0]);
    assert(binary->right_operand == binary->children[1]);
    assert(ast_validate(program, &error));

    size_t saved_end = binary->end_offset;
    binary->end_offset = binary->start_offset - 1;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    binary->end_offset = saved_end;
    assert(ast_validate(program, &error));

    ASTNode *saved_second = binary->children[1];
    binary->children[1] = binary->children[0];
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    binary->children[1] = saved_second;
    assert(ast_validate(program, &error));

    size_t saved_capacity = binary->child_capacity;
    binary->child_capacity = binary->child_count - 1;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    binary->child_capacity = saved_capacity;
    assert(ast_validate(program, &error));

    ASTNode *saved_root_parent = program->parent;
    program->parent = analysis;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    program->parent = saved_root_parent;

    /* AST spans preserve lexer coordinates above INT_MAX without narrowing. */
    Token wide_start = {0};
    Token wide_end = {0};
    size_t wide_position = (size_t)INT_MAX + (size_t)1;
    wide_start.line = wide_position;
    wide_start.column = wide_position;
    wide_start.start_offset = 9;
    wide_end.end_line = wide_position;
    wide_end.end_column = wide_position + (size_t)1;
    wide_end.end_offset = 10;
    ASTNode *wide_span = ast_create(AST_EXPRESION_LITERAL);
    assert(wide_span != NULL);
    assert(ast_set_source_span(wide_span, &wide_start, &wide_end));
    assert(wide_span->line == wide_position);
    assert(wide_span->column == wide_position);
    assert(wide_span->end_line == wide_position);
    assert(wide_span->end_column == wide_position + (size_t)1);
    ast_destroy(wide_span);

    ast_destroy(program);
    parser_release(&parser);

    assert(!ast_validate(NULL, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    return 0;
}
