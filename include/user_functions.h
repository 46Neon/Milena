#ifndef MILENA_USER_FUNCTIONS_H
#define MILENA_USER_FUNCTIONS_H
#include <stdbool.h>
#include <stddef.h>
typedef struct MilenaFunction MilenaFunction;
typedef struct MilenaFunctionValue { double number; } MilenaFunctionValue;
typedef enum { MILENA_FN_NUMBER, MILENA_FN_PARAMETER, MILENA_FN_LOCAL, MILENA_FN_ADD, MILENA_FN_SUB, MILENA_FN_MUL, MILENA_FN_DIV, MILENA_FN_CALL, MILENA_FN_RETURN, MILENA_FN_SEQUENCE, MILENA_FN_LOCAL_DECL } MilenaFunctionNodeType;
typedef struct MilenaFunctionNode { MilenaFunctionNodeType type; double number; size_t index; char *name; struct MilenaFunctionNode **children; size_t child_count; } MilenaFunctionNode;
typedef struct MilenaFunctionParameter { char *name; } MilenaFunctionParameter;
struct MilenaFunction { char *name; MilenaFunctionParameter *parameters; size_t parameter_count; char **local_names; size_t local_count; MilenaFunctionNode *body; };
typedef struct MilenaFunctionTable { MilenaFunction **items; size_t count, capacity; } MilenaFunctionTable;
void milena_function_table_init(MilenaFunctionTable *table);
void milena_function_table_release(MilenaFunctionTable *table);
bool milena_function_table_add(MilenaFunctionTable *table, MilenaFunction *function);
MilenaFunction *milena_function_table_find(const MilenaFunctionTable *table, const char *name);
void milena_function_destroy(MilenaFunction *function);
bool milena_function_call(const MilenaFunctionTable *table, const char *name, const double *arguments, size_t argument_count, double *result, char *error, size_t error_size);
/* Parses the numeric Spanish subset: funcion f(a) { variable x = a + 1; retorna x; } */
bool milena_parse_numeric_functions(const char *source, MilenaFunctionTable *table, char *error, size_t error_size);
#endif
