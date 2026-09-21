#include "entrypoints.h"

#include "analysis.h"
#include "ast.h"
#include "dataset.h"
#include "language_runtime.h"
#include "language_semantic.h"
#include "lexer.h"
#include "parser.h"
#include "schema.h"
#include "script.h"

#include <string.h>

/*
 * The compatibility CLI has no private parser.  This deliberately tiny
 * source is parsed just like a user program; the concrete CLI arguments stay
 * outside the source so paths longer than MAX_TOKEN_LEN retain their old
 * behavior.  The resulting AST is the capability gate for the backend.
 */
static MilenaStatus cli_frontend(ASTNode **program_out, ASTNode **analysis_out,
                                 MilenaError *error) {
    const char *source = ". analisis cli { dataset cargar(\"__cli_input__\") }";
    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    ASTNode *program = parser_parse(&parser);
    if (!program || parser.has_error) {
        if (error) *error = parser.error;
        ast_destroy(program);
        parser_release(&parser);
        return error && error->code != MILENA_OK ? error->code : MILENA_ERR_PARSE;
    }
    MilenaStatus status = milena_validate_ast(program, error);
    if (status != MILENA_OK) {
        ast_destroy(program);
        parser_release(&parser);
        return status;
    }
    ASTNode *analysis = NULL;
    for (size_t i = 0; i < program->child_count; i++) {
        if (program->children[i]->type == AST_BLOQUE_ANALISIS) {
            analysis = program->children[i];
            break;
        }
    }
    if (!analysis) {
        ast_destroy(program);
        parser_release(&parser);
        milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                         "La entrada CLI no produjo un análisis canónico");
        return MILENA_ERR_PARSE;
    }
    *program_out = program;
    *analysis_out = analysis;
    parser_release(&parser);
    return MILENA_OK;
}

static MilenaStatus cli_load_dataset(const char *input_csv, Dataset *dataset,
                                     ASTNode **program, MilenaError *error) {
    if (!input_csv || !dataset || !program) return MILENA_ERR_ARGUMENT;
    ASTNode *analysis = NULL;
    MilenaStatus status = cli_frontend(program, &analysis, error);
    (void)analysis;
    if (status != MILENA_OK) return status;
    dataset_init(dataset);
    status = dataset_load_csv(dataset, input_csv, ',', error);
    if (status != MILENA_OK) {
        ast_destroy(*program);
        *program = NULL;
    }
    return status;
}

MilenaStatus milena_cli_analyze(const char *input_csv, const char *output_json,
                                FILE *output, MilenaError *error) {
    if (error) milena_error_clear(error);
    if (!input_csv || !output_json) return MILENA_ERR_ARGUMENT;
    Dataset dataset = {0};
    ASTNode *program = NULL;
    MilenaStatus status = cli_load_dataset(input_csv, &dataset, &program, error);
    if (status == MILENA_OK) {
        FILE *stream = output ? output : stdout;
        dataset_print(&dataset, 5, stream);
        SalesSummary summary;
        status = analysis_sales(&dataset, "fecha", "precio", "cantidad",
                                output_json, &summary, error);
        if (status == MILENA_OK) {
            fprintf(stream, "Total: %.2f | Promedio: %.2f | Máxima: %.2f | Mínima: %.2f\n",
                    summary.total, summary.average, summary.maximum,
                    summary.minimum);
            fprintf(stream, "Vistas: %zu | Usadas: %zu | Rechazadas: %zu\n",
                    summary.rows_seen, summary.rows_used, summary.rows_rejected);
        }
    }
    if (program) ast_destroy(program);
    dataset_destroy(&dataset);
    return status;
}

MilenaStatus milena_cli_profile(const char *input_csv, const char *output_json,
                                FILE *output, MilenaError *error) {
    if (error) milena_error_clear(error);
    if (!input_csv || !output_json) return MILENA_ERR_ARGUMENT;
    Dataset dataset = {0};
    ASTNode *program = NULL;
    MilenaStatus status = cli_load_dataset(input_csv, &dataset, &program, error);
    MilenaSchema schema;
    schema_init(&schema);
    if (status == MILENA_OK) {
        for (size_t i = 0; i < dataset.column_count; i++) {
            status = schema_add(&schema, dataset.headers[i], MILENA_VAR_TEXT,
                                MILENA_ROLE_FEATURE, error);
            if (status != MILENA_OK) break;
        }
    }
    if (status == MILENA_OK)
        status = analysis_dataset_report(&dataset, &schema, output_json, error);
    schema_destroy(&schema);
    if (program) ast_destroy(program);
    dataset_destroy(&dataset);
    if (status == MILENA_OK)
        fprintf(output ? output : stdout, "Perfil guardado en: %s\n", output_json);
    return status;
}

MilenaStatus milena_cli_inspect(const char *input_csv, FILE *output,
                                MilenaError *error) {
    if (error) milena_error_clear(error);
    if (!input_csv) return MILENA_ERR_ARGUMENT;
    Dataset dataset = {0};
    ASTNode *program = NULL;
    MilenaStatus status = cli_load_dataset(input_csv, &dataset, &program, error);
    if (status == MILENA_OK)
        dataset_print(&dataset, 10, output ? output : stdout);
    if (program) ast_destroy(program);
    dataset_destroy(&dataset);
    return status;
}

MilenaStatus milena_cli_run_script(const char *script_filename,
                                   MilenaError *error) {
    return milena_run_script(script_filename, error);
}
