#ifndef MILENA_AST_H
#define MILENA_AST_H

#include "common.h"
#include "token.h"

typedef enum {
    AST_PROGRAMA,
    AST_BLOQUE_ANALISIS,
    AST_DECLARACION_DATOS,
    AST_DECLARACION_ESTADISTICA,
    AST_ASIGNACION_DATASET,
    AST_LLAMADA_CARGAR,
    AST_BLOQUE_LIMPIAR,
    AST_BLOQUE_TRANSFORMAR,
    AST_BLOQUE_FILTRAR,
    AST_BLOQUE_AGRUPAR,
    AST_BLOQUE_RESUMIR,
    AST_BLOQUE_VISUALIZAR,
    AST_BLOQUE_EXPORTAR,
    AST_EXPRESION_OPERACION,
    AST_EXPRESION_LITERAL,
    AST_EXPRESION_IDENTIFICADOR,
    AST_EXPRESION_FUNCION,
    AST_EXPRESION_ARRAY,
    AST_EXPRESION_LLAMADA,
    AST_BLOQUE_FUNCION,
    AST_COMANDO_RETORNAR,
    AST_CONDICION_SI,
    AST_DECLARACION_FUNCION,
    AST_DECLARACION_ARRAY,
    AST_DECLARACION_VARIABLE,
    AST_ASIGNACION_VARIABLE,
    AST_COMANDO_NULOS,
    AST_COMANDO_DUPLICADOS,
    AST_COMANDO_CONDICION,
    AST_COMANDO_EXTRAER,
    AST_COMANDO_TOTAL,
    AST_COMANDO_PERIODO,
    AST_AGRUPACION_POR,
    AST_AGRUPACION_SPILL,
    AST_RESUMEN_METRICA,
    AST_OPERACION_ESTADISTICA,
    AST_DECLARACION_ENTRADA,
    AST_DECLARACION_SALIDA,
    AST_BLOQUE_SELECCIONAR,
    AST_COMANDO_COLUMNAS,
    AST_BLOQUE_UNIR,
    AST_COMANDO_DERECHA,
    AST_COMANDO_CLAVE,
    AST_COMANDO_SST,
    AST_STREAM_FILTER,
    AST_COLUMNAR_PROJECT,
    AST_COLUMNAR_FIELD,
    AST_SQL_PROGRAM,
    AST_SQL_QUERY,
    AST_SQL_EXECUTE,
    AST_SQL_BEGIN,
    AST_SQL_COMMIT,
    AST_SQL_ROLLBACK,
    AST_SQL_PARAMETER,
    AST_SQL_TABLE_SCHEMA,
    AST_SQL_SCHEMA_COLUMN,
    AST_SQL_TYPED_SELECT,
    AST_SQL_TABLE_REFERENCE,
    AST_SQL_PROJECTION_LIST,
    AST_SQL_PROJECTED_COLUMN,
    AST_SQL_FILTER,
    AST_SQL_FILTER_COLUMN,
    AST_SQL_FILTER_OPERATOR,
    AST_SQL_TYPED_INSERT,
    AST_SQL_INSERT_COLUMN_LIST,
    AST_SQL_INSERT_COLUMN,
    AST_SQL_INSERT_VALUE_LIST,
    AST_SQL_TYPED_UPDATE,
    AST_SQL_UPDATE_ASSIGNMENT_LIST,
    AST_SQL_UPDATE_ASSIGNMENT,
    AST_SQL_UPDATE_COLUMN,
    AST_SQL_UPDATE_FILTER,
    AST_NODE_TYPE_COUNT
} ASTNodeType;

typedef enum {
    AST_SQL_TYPE_UNSPECIFIED = 0,
    AST_SQL_TYPE_INTEGER,
    AST_SQL_TYPE_REAL,
    AST_SQL_TYPE_TEXT,
    AST_SQL_TYPE_BOOLEAN
} ASTSqlType;

typedef enum {
    AST_SQL_OPERATOR_UNSPECIFIED = 0,
    AST_SQL_OPERATOR_EQUAL,
    AST_SQL_OPERATOR_NOT_EQUAL,
    AST_SQL_OPERATOR_LESS,
    AST_SQL_OPERATOR_LESS_EQUAL,
    AST_SQL_OPERATOR_GREATER,
    AST_SQL_OPERATOR_GREATER_EQUAL
} ASTSqlOperator;

typedef enum {
    AST_STREAM_FILTER_TEXT_EQUAL = 0,
    AST_STREAM_FILTER_NUMERIC_GREATER
} ASTStreamFilterKind;

/* Static value annotations used by the typed scalar-expression frontend. */
typedef enum {
    AST_VALUE_UNRESOLVED = 0,
    AST_VALUE_NUMBER,
    AST_VALUE_BOOLEAN,
    AST_VALUE_TEXT,
    AST_VALUE_ARRAY,
    AST_VALUE_DATASET,
    AST_VALUE_TYPE_COUNT
} ASTValueType;

typedef enum {
    AST_OPERATOR_NONE = 0,
    AST_OPERATOR_ADD,
    AST_OPERATOR_SUBTRACT,
    AST_OPERATOR_MULTIPLY,
    AST_OPERATOR_DIVIDE,
    AST_OPERATOR_EQUAL,
    AST_OPERATOR_NOT_EQUAL,
    AST_OPERATOR_GREATER,
    AST_OPERATOR_GREATER_EQUAL,
    AST_OPERATOR_LESS,
    AST_OPERATOR_LESS_EQUAL,
    AST_OPERATOR_COUNT
} ASTOperatorKind;

typedef enum {
    AST_ESTADISTICA_NINGUNA,
    AST_ESTADISTICA_SUMA,
    AST_ESTADISTICA_MEDIA,
    AST_ESTADISTICA_MINIMO,
    AST_ESTADISTICA_MAXIMO,
    AST_ESTADISTICA_VARIANZA,
    AST_ESTADISTICA_DESVIACION,
    AST_ESTADISTICA_MEDIANA,
    AST_ESTADISTICA_PERCENTIL,
    AST_STAT_OPERATION_COUNT
} ASTStatOperation;

typedef enum {
    AST_STREAM_OPERATION_NONE,
    AST_STREAM_OPERATION_SUM,
    AST_STREAM_OPERATION_MEAN,
    AST_STREAM_OPERATION_MIN,
    AST_STREAM_OPERATION_MAX,
    AST_STREAM_OPERATION_COUNT,
    AST_STREAM_OPERATION_VARIANCE,
    AST_STREAM_OPERATION_STDDEV
} ASTStreamOperation;

typedef enum {
    AST_AGGREGATE_OPERATION_NONE = 0,
    AST_AGGREGATE_OPERATION_SUM,
    AST_AGGREGATE_OPERATION_MEAN,
    AST_AGGREGATE_OPERATION_MIN,
    AST_AGGREGATE_OPERATION_MAX,
    AST_AGGREGATE_OPERATION_COUNT,
    AST_AGGREGATE_OPERATION_VARIANCE,
    AST_AGGREGATE_OPERATION_STDDEV,
    AST_AGGREGATE_OPERATION_MEDIAN,
    AST_AGGREGATE_OPERATION_PERCENTILE,
    AST_AGGREGATE_OPERATION_LIMIT
} ASTAggregateOperation;

typedef enum {
    AST_DATA_COLUMN_TYPE_UNSPECIFIED = 0,
    AST_DATA_COLUMN_TYPE_NUMERIC,
    AST_DATA_COLUMN_TYPE_BINARY,
    AST_DATA_COLUMN_TYPE_TEXT,
    AST_DATA_COLUMN_TYPE_DATE,
    AST_DATA_COLUMN_TYPE_CATEGORICAL,
    AST_DATA_COLUMN_TYPE_COUNT
} ASTDataColumnType;

typedef struct {
    char *path;
    bool present;
} ASTDataSourcePayload;

typedef struct {
    char *name;
    ASTDataColumnType type;
    bool present;
} ASTDataColumnPayload;

typedef struct {
    char *destination;
    bool present;
} ASTExportPayload;

typedef struct {
    char *path;
    bool present;
} ASTJoinRightPayload;

typedef struct {
    char *name;
    bool present;
} ASTJoinKeyPayload;

typedef struct {
    char *left_column;
    char *right_column;
    bool present;
} ASTDataProductPayload;

typedef struct {
    char **names;
    size_t count;
    bool present;
} ASTColumnSelectionPayload;

typedef enum {
    AST_DATA_CLEANUP_ACTION_NONE = 0,
    AST_DATA_CLEANUP_ACTION_REMOVE
} ASTDataCleanupAction;

typedef struct {
    ASTDataCleanupAction action;
    bool present;
} ASTDataCleanupPayload;

typedef struct {
    char *name;
    bool present;
} ASTGroupKeyPayload;

typedef enum {
    AST_FILTER_PREDICATE_OK = 0,
    AST_FILTER_PREDICATE_INVALID,
    AST_FILTER_PREDICATE_MEMORY
} ASTFilterPredicateStatus;

typedef struct ASTNode {
    ASTNodeType type;
    ASTStatOperation statistical_operation;
    /* Semantic annotation; unresolved until the typed frontend pass. */
    ASTValueType value_type;
    /* Nonzero ID assigned by canonical script name resolution; never an owner. */
    size_t resolved_symbol_id;
    /* Structured scalar-expression operator, independent of legacy value text. */
    ASTOperatorKind operator_kind;
    /* Structured numeric-filter payload; filter_column is owned by this node. */
    ASTOperatorKind filter_operator;
    char *filter_column;
    double filter_threshold;
    bool has_filter_predicate;
    /* Typed canonical group/summary metric; aggregate_column is owned. */
    ASTAggregateOperation aggregate_operation;
    char *aggregate_column;
    bool has_aggregate_metric;
    /* Owned typed payloads; legacy value/type_name remain compatibility mirrors. */
    ASTDataSourcePayload data_source;
    ASTDataColumnPayload data_column;
    ASTExportPayload export_result;
    /* Owned typed join operands; value remains a validated compatibility mirror. */
    ASTJoinRightPayload join_right;
    ASTJoinKeyPayload join_key;
    /* Owned typed operands for #total; value remains a validated mirror. */
    ASTDataProductPayload data_product;
    /* Owned selection names for #columnas; value is a validated mirror. */
    ASTColumnSelectionPayload column_selection;
    /* Enumerated operation for admitted null/duplicate cleanup commands. */
    ASTDataCleanupPayload data_cleanup;
    /* Owned typed #por key; value remains a validated compatibility mirror. */
    ASTGroupKeyPayload group_key;
    /* Non-owning aliases of children[0] and children[1] for binary operators. */
    struct ASTNode *left_operand;
    struct ASTNode *right_operand;
    char *value;
    char *type_name;
    double number_value;
    double percentile;
    int axis;
    bool keepdims;
    bool zeros_constructor;
    /* Contrato explícito de flujo; cero significa valor predeterminado. */
    ASTStreamOperation stream_operation;
    ASTStreamFilterKind stream_filter_kind;
    size_t stream_chunk_rows;
    size_t stream_batch_limit_bytes;
    size_t stream_record_limit;
    size_t stream_column_limit;
    size_t stream_group_limit;
    size_t stream_row_limit;
    size_t stream_input_limit_bytes;
    size_t stream_output_limit_bytes;
    double stream_time_limit_ms;
    /* Explicit resource policy for canonical #agrupar spill-to-disk. */
    size_t group_memory_budget_bytes;
    size_t group_spill_quota_bytes;
    size_t group_max_key_bytes;
    size_t group_max_output_groups;
    size_t group_max_output_bytes;
    size_t group_max_runs;
    bool group_output_limit_explicit;
    /* Resource limits for canonical table joins. */
    size_t join_memory_budget_bytes;
    size_t join_max_output_rows;
    bool join_limits_explicit;
    /* SQL execution budgets; zero selects the documented default. */
    size_t sql_max_rows;
    size_t sql_max_bytes;
    size_t sql_timeout_ms;
    bool sql_limits_explicit;
    /* Explicit typed SQL/ORM AST annotations; these are not raw SQL text. */
    ASTSqlType sql_type;
    ASTSqlOperator sql_operator;
    struct ASTNode **children;
    size_t child_count;
    size_t child_capacity;
    int line;
    int column;
    int end_line;
    int end_column;
    size_t start_offset;
    size_t end_offset;
    bool has_source_span;
    struct ASTNode *parent;
} ASTNode;

ASTNode* ast_create(ASTNodeType type);
/* Validate ownership, parent links, child storage, types and source-span containment. */
bool ast_validate(const ASTNode *root, MilenaError *error);
bool ast_set_source_span(ASTNode *node, const Token *start, const Token *end);
bool ast_set_source_span_from_nodes(ASTNode *node, const ASTNode *first,
                                    const ASTNode *last);
/* Parse and own the strict numeric predicate while retaining the legacy value text. */
ASTFilterPredicateStatus ast_set_filter_predicate(ASTNode *node,
                                                   const char *text);
/* Set typed canonical metric data while preserving the legacy value spelling. */
bool ast_set_aggregate_metric(ASTNode *node, ASTAggregateOperation operation,
                              const char *column);
/* Typed, independently owned mirrors for the admitted canonical data-summary nodes. */
bool ast_set_data_source_declaration(ASTNode *node, const char *path);
bool ast_set_data_column_declaration(ASTNode *node, const char *name,
                                     ASTDataColumnType type);
bool ast_set_export_destination(ASTNode *node, const char *destination);
bool ast_set_join_right_source(ASTNode *node, const char *path);
bool ast_set_join_key(ASTNode *node, const char *name);
bool ast_set_data_product(ASTNode *node, const char *expression);
bool ast_set_column_selection(ASTNode *node, const char *names);
bool ast_set_data_cleanup_action(ASTNode *node, ASTDataCleanupAction action);
bool ast_set_group_key(ASTNode *node, const char *name);
const char *ast_data_column_type_name(ASTDataColumnType type);
ASTNode* ast_create_leaf(ASTNodeType type, const char *value);
ASTNode* ast_create_number(double value);
ASTNode* ast_create_statistic(ASTStatOperation operation, ASTNode *argument,
                              int axis, bool keepdims, double percentile);
bool ast_add_child(ASTNode *parent, ASTNode *child);
void ast_print(ASTNode *node, int depth);
void ast_destroy(ASTNode *node);
const char* ast_type_name(ASTNodeType type);
const char *ast_stat_operation_name(ASTStatOperation operation);
ASTOperatorKind ast_operator_kind_from_name(const char *name);
const char *ast_operator_kind_name(ASTOperatorKind operation);
const char *ast_value_type_name(ASTValueType type);

#endif
