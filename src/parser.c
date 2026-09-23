#include "parser.h"

void parser_init(Parser *parser, Lexer *lexer) {
    parser->lexer = lexer;
    parser->current = lexer_next_token(lexer);
    parser->previous = parser->current;
    parser->has_error = false;
    milena_error_init(&parser->error);
    milena_symbols_init(&parser->symbols);
}

void parser_release(Parser *parser) {
    if (!parser) return;
    milena_symbols_release(&parser->symbols);
}

void parser_error(Parser *parser, const char *msg) {
    milena_error_set(&parser->error, MILENA_ERR_PARSE,
                     parser->current.line, parser->current.column, 0, msg);
    parser->has_error = true;
}

void parser_advance(Parser *parser) {
    parser->previous = parser->current;
    parser->current = lexer_next_token(parser->lexer);
}

bool parser_match(Parser *parser, TokenType type) {
    return parser->current.type == type;
}

static bool parser_is_identifier(Parser *parser) {
    if (!parser) return false;
    return parser_match(parser, TOKEN_IDENTIFICADOR) ||
           parser_match(parser, TOKEN_KW_TOTAL);
}

bool parser_expect(Parser *parser, TokenType type, const char *msg) {
    if (!parser_match(parser, type)) {
        parser_error(parser, msg);
        return false;
    }
    parser_advance(parser);
    return true;
}

/* AST construction is fallible. Centralize ownership/error handling so an
 * allocation failure cannot silently produce a truncated program. On failure
 * this function consumes child, just like ast_destroy does for an owned node. */
static bool parser_add_child(Parser *parser, ASTNode *parent, ASTNode *child,
                             const char *message) {
    if (ast_add_child(parent, child)) return true;
    ast_destroy(child);
    parser_error(parser, message);
    return false;
}

static ASTNode *parse_array_declaration(Parser *parser) {
    if (!parser || !parser_is_identifier(parser) ||
        (strcmp(parser->current.lexeme, "array") != 0 &&
         strcmp(parser->current.lexeme, "arreglo") != 0)) return NULL;
    parser_advance(parser);

    if (!parser_is_identifier(parser)) {
        parser_error(parser, "Se esperaba nombre del array");
        return NULL;
    }
    char name[MAX_TOKEN_LEN];
    strncpy(name, parser->current.lexeme, sizeof(name) - 1);
    parser_advance(parser);
    name[sizeof(name) - 1] = '\0';
    if (!parser_expect(parser, TOKEN_IGUAL,
                       "Se esperaba '=' en la declaración del array")) {
        return NULL;
    }

    ASTNode *array = ast_create(AST_EXPRESION_ARRAY);
    if (!array) {
        parser_error(parser, "No se pudo crear el literal del array");
        return NULL;
    }

    bool zeros_constructor = parser_is_identifier(parser) &&
        (strcmp(parser->current.lexeme, "ceros") == 0 ||
         strcmp(parser->current.lexeme, "zeros") == 0);
    if (zeros_constructor) {
        array->zeros_constructor = true;
        parser_advance(parser);
        if (!parser_expect(parser, TOKEN_PAR_IZQ,
                           "Se esperaba '(' después de ceros")) {
            ast_destroy(array);
            return NULL;
        }
        if (parser_match(parser, TOKEN_PAR_DER)) {
            ast_destroy(array);
            parser_error(parser, "ceros requiere al menos una dimensión");
            return NULL;
        }
        while (true) {
            if (!parser_expect(parser, TOKEN_NUMERO,
                               "La dimensión de ceros debe ser numérica")) {
                ast_destroy(array);
                return NULL;
            }
            double dimension = parser->previous.number_value;
            if (!isfinite(dimension) || dimension <= 0.0 ||
                dimension > (double)SIZE_MAX || floor(dimension) != dimension) {
                ast_destroy(array);
                parser_error(parser, "Las dimensiones de ceros deben ser enteros positivos");
                return NULL;
            }
            ASTNode *number = ast_create_number(dimension);
            if (!number) {
                ast_destroy(array);
                parser_error(parser, "No se pudo crear una dimensión de ceros");
                return NULL;
            }
            if (!parser_add_child(parser, array, number,
                                  "No se pudo conectar una dimensión de ceros")) {
                ast_destroy(array);
                return NULL;
            }
            if (parser_match(parser, TOKEN_PAR_DER)) {
                parser_advance(parser);
                break;
            }
            if (!parser_expect(parser, TOKEN_COMA,
                               "Se esperaba ',' entre dimensiones")) {
                ast_destroy(array);
                return NULL;
            }
            if (parser_match(parser, TOKEN_PAR_DER)) {
                ast_destroy(array);
                parser_error(parser, "No se admite una coma final en ceros");
                return NULL;
            }
        }
    } else {
        if (!parser_expect(parser, TOKEN_CORCHETE_IZQ,
                           "Se esperaba '[' en el literal del array")) {
            ast_destroy(array);
            return NULL;
        }
        if (parser_match(parser, TOKEN_CORCHETE_DER)) {
            ast_destroy(array);
            parser_error(parser, "Un literal de array no puede estar vacío");
            return NULL;
        }
        while (true) {
            if (!parser_expect(parser, TOKEN_NUMERO,
                               "El literal de array solo admite números")) {
                ast_destroy(array);
                return NULL;
            }
            ASTNode *number = ast_create_number(parser->previous.number_value);
            if (!number) {
                ast_destroy(array);
                parser_error(parser, "No se pudo crear un elemento del array");
                return NULL;
            }
            if (!parser_add_child(parser, array, number,
                                  "No se pudo conectar un elemento del array")) {
                ast_destroy(array);
                return NULL;
            }
            if (parser_match(parser, TOKEN_CORCHETE_DER)) {
                parser_advance(parser);
                break;
            }
            if (!parser_expect(parser, TOKEN_COMA,
                               "Se esperaba ',' entre elementos del array")) {
                ast_destroy(array);
                return NULL;
            }
            if (parser_match(parser, TOKEN_CORCHETE_DER)) {
                ast_destroy(array);
                parser_error(parser, "No se admite coma final en el array");
                return NULL;
            }
        }
    }

    if (!parser_expect(parser, TOKEN_PUNTO_Y_COMA,
                       "Se esperaba ';' después del array")) {
        ast_destroy(array);
        return NULL;
    }

    ASTNode *declaration = ast_create_leaf(AST_DECLARACION_ARRAY, name);
    if (!declaration) {
        ast_destroy(array);
        parser_error(parser, "No se pudo crear la declaración del array");
        return NULL;
    }
    if (milena_symbols_declare(&parser->symbols, name, &parser->error) != MILENA_OK) {
        ast_destroy(declaration);
        ast_destroy(array);
        parser->has_error = true;
        return NULL;
    }
    if (!parser_add_child(parser, declaration, array,
                          "No se pudo conectar el literal al AST")) {
        ast_destroy(declaration);
        return NULL;
    }
    return declaration;
}

static ASTNode *parse_expression(Parser *parser) {
    ASTNode *left = NULL;
    if (parser_match(parser, TOKEN_NUMERO)) {
        parser_advance(parser);
        left = ast_create_number(parser->previous.number_value);
    } else if (parser_is_identifier(parser)) {
        if (!milena_symbols_exists(&parser->symbols, parser->current.lexeme)) {
            parser_error(parser, "La variable usada no ha sido declarada");
            return NULL;
        }
        left = ast_create_leaf(AST_EXPRESION_IDENTIFICADOR, parser->current.lexeme);
        parser_advance(parser);
    } else {
        parser_error(parser, "Se esperaba una expresión numérica");
        return NULL;
    }
    if (!left) {
        parser_error(parser, "No se pudo crear la expresión");
        return NULL;
    }

    while (parser_match(parser, TOKEN_MAS) || parser_match(parser, TOKEN_MENOS)) {
        TokenType operator_type = parser->current.type;
        parser_advance(parser);
        ASTNode *right = NULL;
        if (parser_match(parser, TOKEN_NUMERO)) {
            parser_advance(parser);
            right = ast_create_number(parser->previous.number_value);
        } else if (parser_is_identifier(parser)) {
            if (!milena_symbols_exists(&parser->symbols, parser->current.lexeme)) {
                ast_destroy(left);
                parser_error(parser, "La variable usada no ha sido declarada");
                return NULL;
            }
            right = ast_create_leaf(AST_EXPRESION_IDENTIFICADOR, parser->current.lexeme);
            parser_advance(parser);
        } else {
            ast_destroy(left);
            parser_error(parser, "Se esperaba un valor después del operador");
            return NULL;
        }
        ASTNode *operation = ast_create_leaf(AST_EXPRESION_OPERACION,
                                             operator_type == TOKEN_MAS ? "+" : "-");
        if (!operation || !right) {
            ast_destroy(left);
            ast_destroy(right);
            ast_destroy(operation);
            parser_error(parser, "No se pudo crear la expresión");
            return NULL;
        }
        if (!parser_add_child(parser, operation, left,
                              "Sin memoria para el operando izquierdo")) {
            ast_destroy(right);
            ast_destroy(operation);
            return NULL;
        }
        if (!parser_add_child(parser, operation, right,
                              "Sin memoria para el operando derecho")) {
            ast_destroy(operation);
            return NULL;
        }
        left = operation;
    }
    return left;
}

static bool parser_is_schema_type(const char *text) {
    return text && (strcmp(text, "numerica") == 0 ||
                    strcmp(text, "categorica") == 0 ||
                    strcmp(text, "binaria") == 0 ||
                    strcmp(text, "texto") == 0 ||
                    strcmp(text, "fecha") == 0);
}

static ASTNode *parse_variable_declaration(Parser *parser) {
    parser_advance(parser);
    if (!parser_is_identifier(parser)) {
        parser_error(parser, "Se esperaba nombre de variable");
        return NULL;
    }
    char name[MAX_TOKEN_LEN];
    strncpy(name, parser->current.lexeme, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    parser_advance(parser);

    /* Declaración de columna de un dataset: variable edad numerica */
    if (parser_is_identifier(parser) &&
        parser_is_schema_type(parser->current.lexeme)) {
        char type_name[MAX_TOKEN_LEN];
        strncpy(type_name, parser->current.lexeme, sizeof(type_name) - 1);
        type_name[sizeof(type_name) - 1] = '\0';
        parser_advance(parser);
        if (parser_match(parser, TOKEN_PUNTO_Y_COMA)) parser_advance(parser);
        ASTNode *node = ast_create_leaf(AST_DECLARACION_VARIABLE, name);
        if (!node) {
            parser_error(parser, "No se pudo crear la declaración de columna");
            return NULL;
        }
        node->type_name = milena_strdup(type_name);
        if (!node->type_name ||
            milena_symbols_declare(&parser->symbols, name, &parser->error) != MILENA_OK) {
            ast_destroy(node);
            parser->has_error = true;
            return NULL;
        }
        return node;
    }

    if (!parser_expect(parser, TOKEN_IGUAL,
                       "Se esperaba '=' en la declaración de variable")) return NULL;
    ASTNode *value = parse_expression(parser);
    if (!value) return NULL;
    if (milena_symbols_declare(&parser->symbols, name, &parser->error) != MILENA_OK) {
        parser->has_error = true;
        ast_destroy(value);
        return NULL;
    }
    ASTNode *node = ast_create_leaf(AST_DECLARACION_VARIABLE, name);
    if (!node) {
        ast_destroy(value);
        parser_error(parser, "No se pudo crear la variable");
        return NULL;
    }
    if (!parser_add_child(parser, node, value,
                          "No se pudo conectar la variable al AST")) {
        ast_destroy(node);
        return NULL;
    }
    if (!parser_expect(parser, TOKEN_PUNTO_Y_COMA,
                       "Se esperaba ';' después de la variable")) {
        ast_destroy(node);
        return NULL;
    }
    return node;
}

static ASTNode *parse_assignment(Parser *parser) {
    char name[MAX_TOKEN_LEN];
    strncpy(name, parser->current.lexeme, sizeof(name) - 1); name[sizeof(name) - 1] = '\0';
    if (!milena_symbols_exists(&parser->symbols, name)) {
        parser_error(parser, "La variable asignada no ha sido declarada");
        return NULL;
    }
    parser_advance(parser);
    if (!parser_expect(parser, TOKEN_IGUAL, "Se esperaba '=' en la asignación")) return NULL;
    ASTNode *value = parse_expression(parser);
    if (!value) return NULL;
    ASTNode *node = ast_create_leaf(AST_ASIGNACION_VARIABLE, name);
    if (!node) {
        ast_destroy(value);
        parser_error(parser, "No se pudo crear la asignación");
        return NULL;
    }
    if (!parser_add_child(parser, node, value,
                          "Sin memoria para conectar la asignación")) {
        ast_destroy(node);
        return NULL;
    }
    if (!parser_expect(parser, TOKEN_PUNTO_Y_COMA, "Se esperaba ';' después de la asignación")) {
        ast_destroy(node);
        return NULL;
    }
    return node;
}

static bool parser_is_statistical_token(TokenType type) {
    return type == TOKEN_FUNCION_SUMA ||
           type == TOKEN_FUNCION_MEDIA ||
           type == TOKEN_FUNCION_MINIMO ||
           type == TOKEN_FUNCION_MAXIMO ||
           type == TOKEN_FUNCION_VARIANZA ||
           type == TOKEN_FUNCION_DESVIACION ||
           type == TOKEN_FUNCION_MEDIANA ||
           type == TOKEN_FUNCION_PERCENTIL;
}

static ASTStatOperation parser_statistical_operation(TokenType type) {
    switch (type) {
        case TOKEN_FUNCION_SUMA: return AST_ESTADISTICA_SUMA;
        case TOKEN_FUNCION_MEDIA: return AST_ESTADISTICA_MEDIA;
        case TOKEN_FUNCION_MINIMO: return AST_ESTADISTICA_MINIMO;
        case TOKEN_FUNCION_MAXIMO: return AST_ESTADISTICA_MAXIMO;
        case TOKEN_FUNCION_VARIANZA: return AST_ESTADISTICA_VARIANZA;
        case TOKEN_FUNCION_DESVIACION: return AST_ESTADISTICA_DESVIACION;
        case TOKEN_FUNCION_MEDIANA: return AST_ESTADISTICA_MEDIANA;
        case TOKEN_FUNCION_PERCENTIL: return AST_ESTADISTICA_PERCENTIL;
        default: return AST_ESTADISTICA_NINGUNA;
    }
}

static ASTNode *parse_statistical_call(Parser *parser,
                                       bool require_declared_symbol) {
    if (!parser || !parser_is_statistical_token(parser->current.type)) {
        if (parser) parser_error(parser, "Se esperaba una operación estadística");
        return NULL;
    }

    ASTStatOperation operation = parser_statistical_operation(parser->current.type);
    parser_advance(parser);
    if (!parser_expect(parser, TOKEN_PAR_IZQ,
                       "Se esperaba '(' después de la operación")) {
        return NULL;
    }
    if (!parser_is_identifier(parser)) {
        parser_error(parser, "Se esperaba un arreglo como argumento");
        return NULL;
    }

    char symbol[MAX_TOKEN_LEN];
    strncpy(symbol, parser->current.lexeme, sizeof(symbol) - 1);
    symbol[sizeof(symbol) - 1] = '\0';
    parser_advance(parser);
    if (require_declared_symbol &&
        !milena_symbols_exists(&parser->symbols, symbol)) {
        parser_error(parser, "El arreglo usado no ha sido declarado");
        return NULL;
    }

    ASTNode *argument = ast_create_leaf(AST_EXPRESION_IDENTIFICADOR, symbol);
    if (!argument) {
        parser_error(parser, "No se pudo crear el argumento estadístico");
        return NULL;
    }

    double percentile = 0.0;
    int axis = -1;
    bool keepdims = false;
    bool axis_seen = false;
    bool keepdims_seen = false;

    if (operation == AST_ESTADISTICA_PERCENTIL) {
        if (!parser_expect(parser, TOKEN_COMA,
                           "percentil requiere un valor entre 0 y 100") ||
            !parser_expect(parser, TOKEN_NUMERO,
                           "Se esperaba un percentil numérico")) {
            ast_destroy(argument);
            return NULL;
        }
        percentile = parser->previous.number_value;
        if (!isfinite(percentile) || percentile < 0.0 || percentile > 100.0) {
            ast_destroy(argument);
            parser_error(parser, "El percentil debe estar entre 0 y 100");
            return NULL;
        }
    }

    while (!parser_match(parser, TOKEN_PAR_DER) && !parser->has_error) {
        if (!parser_expect(parser, TOKEN_COMA,
                           "Se esperaba ',' entre argumentos")) break;
        if (parser_match(parser, TOKEN_CONCEPTO_EJE)) {
            if (axis_seen) {
                parser_error(parser, "El eje solo puede indicarse una vez");
                break;
            }
            axis_seen = true;
            parser_advance(parser);
            if (!parser_expect(parser, TOKEN_NUMERO,
                               "Se esperaba un eje entero no negativo")) break;
            double value = parser->previous.number_value;
            if (!isfinite(value) || value < 0.0 || value > (double)INT_MAX ||
                floor(value) != value) {
                parser_error(parser, "El eje debe ser un entero no negativo");
                break;
            }
            axis = (int)value;
        } else if (parser_match(parser, TOKEN_CONCEPTO_CONSERVAR) ||
                   parser_match(parser, TOKEN_CONCEPTO_SIN)) {
            if (keepdims_seen) {
                parser_error(parser,
                             "La opción de dimensiones solo puede indicarse una vez");
                break;
            }
            keepdims_seen = true;
            bool negate = parser_match(parser, TOKEN_CONCEPTO_SIN);
            parser_advance(parser);
            if (negate && !parser_expect(parser, TOKEN_CONCEPTO_CONSERVAR,
                                         "Se esperaba 'conservar' después de 'sin'")) {
                break;
            }
            if (!axis_seen) {
                parser_error(parser,
                             "Se debe indicar un eje antes de conservar dimensiones");
                break;
            }
            if (!(parser_match(parser, TOKEN_FUNCION_DIMENSIONES) ||
                  parser_match(parser, TOKEN_CONCEPTO_DIMENSIONES))) {
                parser_error(parser,
                             "Se esperaba 'dimensiones' en la opción");
                break;
            }
            parser_advance(parser);
            keepdims = !negate;
        } else {
            parser_error(parser, "Argumento estadístico inesperado");
            break;
        }
    }

    if (parser->has_error ||
        !parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')'")) {
        ast_destroy(argument);
        return NULL;
    }
    return ast_create_statistic(operation, argument, axis, keepdims, percentile);
}

static bool parser_expect_word(Parser *parser, const char *word, const char *msg) {
    if (!parser || !word || !parser_is_identifier(parser) ||
        strcmp(parser->current.lexeme, word) != 0) {
        if (parser) parser_error(parser, msg);
        return false;
    }
    parser_advance(parser);
    return true;
}

static bool parser_is_stream_metric(Parser *parser) {
    if (!parser) return false;
    return parser_match(parser, TOKEN_FUNCION_SUMA) ||
           parser_match(parser, TOKEN_FUNCION_MEDIA) ||
           parser_match(parser, TOKEN_FUNCION_MINIMO) ||
           parser_match(parser, TOKEN_FUNCION_MAXIMO) ||
           parser_match(parser, TOKEN_FUNCION_VARIANZA) ||
           parser_match(parser, TOKEN_FUNCION_DESVIACION) ||
           (parser_match(parser, TOKEN_KW_CONTAR));
}

static ASTStreamOperation stream_operation_from_token(Parser *parser) {
    if (!parser) return AST_STREAM_OPERATION_NONE;
    if (parser_match(parser, TOKEN_FUNCION_SUMA)) return AST_STREAM_OPERATION_SUM;
    if (parser_match(parser, TOKEN_FUNCION_MEDIA)) return AST_STREAM_OPERATION_MEAN;
    if (parser_match(parser, TOKEN_FUNCION_MINIMO)) return AST_STREAM_OPERATION_MIN;
    if (parser_match(parser, TOKEN_FUNCION_MAXIMO)) return AST_STREAM_OPERATION_MAX;
    if (parser_match(parser, TOKEN_KW_CONTAR)) return AST_STREAM_OPERATION_COUNT;
    if (parser_match(parser, TOKEN_FUNCION_VARIANZA)) return AST_STREAM_OPERATION_VARIANCE;
    if (parser_match(parser, TOKEN_FUNCION_DESVIACION)) return AST_STREAM_OPERATION_STDDEV;
    return AST_STREAM_OPERATION_NONE;
}

/* Forma legible: resumir { suma de "importe"; contar de "importe"; }.
 * Cada métrica queda tipada en el AST; el runtime no vuelve a escanearla. */
static ASTNode *parse_stream_summary(Parser *parser) {
    parser_advance(parser); /* consume the natural keyword 'resumir' */
    if (!parser_expect(parser, TOKEN_LLAVE_IZQ,
                       "Se esperaba '{' después de resumir")) return NULL;
    ASTNode *summary = ast_create(AST_BLOQUE_RESUMIR);
    if (!summary) { parser_error(parser, "Sin memoria para resumir"); return NULL; }
    while (!parser_match(parser, TOKEN_LLAVE_DER) &&
           !parser_match(parser, TOKEN_EOF) && !parser->has_error) {
        if (!parser_is_stream_metric(parser)) {
            parser_error(parser, "Se esperaba una métrica como 'suma de \\\"columna\\\"'");
            break;
        }
        ASTStreamOperation operation = stream_operation_from_token(parser);
        parser_advance(parser);
        if (!parser_expect_word(parser, "de",
                                "Se esperaba 'de' después de la métrica")) break;
        if (!parser_expect(parser, TOKEN_CADENA,
                           "Se esperaba el nombre de columna entre comillas")) break;
        ASTNode *metric_node = ast_create_leaf(AST_RESUMEN_METRICA,
                                               parser->previous.lexeme);
        if (metric_node) metric_node->stream_operation = operation;
        if (!metric_node) {
            parser_error(parser, "Sin memoria para métrica de resumen");
            break;
        }
        if (!parser_add_child(parser, summary, metric_node,
                              "Sin memoria para métrica de resumen")) break;
        if (parser_match(parser, TOKEN_PUNTO_Y_COMA)) parser_advance(parser);
    }
    if (!parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}' después del resumen") ||
        summary->child_count == 0) {
        if (!parser->has_error) parser_error(parser, "El resumen debe tener al menos una métrica");
        ast_destroy(summary);
        return NULL;
    }
    return summary;
}

static ASTNode *parse_stream_group(Parser *parser) {
    parser_advance(parser); /* consume 'agrupar' */
    if (!parser_expect(parser, TOKEN_KW_POR,
                       "Se esperaba 'por' después de agrupar")) return NULL;
    if (!parser_expect(parser, TOKEN_CADENA,
                       "Se esperaba el nombre de la columna de agrupación entre comillas"))
        return NULL;
    ASTNode *group = ast_create(AST_BLOQUE_AGRUPAR);
    if (!group) {
        parser_error(parser, "Sin memoria para crear la agrupación de flujo");
        return NULL;
    }
    group->type_name = milena_strdup("flujo");
    ASTNode *key = ast_create_leaf(AST_AGRUPACION_POR, parser->previous.lexeme);
    if (!group->type_name || !key) {
        ast_destroy(key);
        ast_destroy(group);
        parser_error(parser, "Sin memoria para la clave de agrupación");
        return NULL;
    }
    if (!parser_add_child(parser, group, key,
                          "Sin memoria para la clave de agrupación")) {
        ast_destroy(group);
        return NULL;
    }
    ASTNode *summary = parse_stream_summary(parser);
    if (!summary) {
        ast_destroy(group);
        return NULL;
    }
    if (!parser_add_child(parser, group, summary,
                          "Sin memoria para métricas agrupadas")) {
        ast_destroy(group);
        return NULL;
    }
    return group;
}

static ASTNode *parse_human_stream_load(Parser *parser) {
    if (!parser_expect(parser, TOKEN_KW_DATOS, "Se esperaba 'datos'")) return NULL;
    if (!parser_expect(parser, TOKEN_KW_DESDE, "Se esperaba 'desde' después de datos")) return NULL;
    if (!parser_expect(parser, TOKEN_CADENA, "Se esperaba la ruta del CSV entre comillas")) return NULL;
    ASTNode *load = ast_create_leaf(AST_LLAMADA_CARGAR, parser->previous.lexeme);
    if (!load) { parser_error(parser, "Sin memoria para cargar datos"); return NULL; }
    load->type_name = milena_strdup("flujo");
    load->stream_chunk_rows = 4096u;
    if (!load->type_name) { ast_destroy(load); parser_error(parser, "Sin memoria para modo flujo"); return NULL; }
    if (parser_match(parser, TOKEN_KW_PROCESAR)) {
        parser_advance(parser);
        if (!parser_expect(parser, TOKEN_KW_POR, "Se esperaba 'por' en 'procesar por lotes'")) goto fail;
        if (!parser_expect(parser, TOKEN_KW_LOTES, "Se esperaba 'lotes'")) goto fail;
        if (!parser_expect_word(parser, "de", "Se esperaba 'de' antes del tamaño del lote")) goto fail;
        if (!parser_expect(parser, TOKEN_NUMERO, "El tamaño del lote debe ser numérico")) goto fail;
        double chunk = parser->previous.number_value;
        if (!isfinite(chunk) || chunk < 1.0 || chunk > 1000000.0 || floor(chunk) != chunk) {
            parser_error(parser, "El tamaño del lote debe ser un entero entre 1 y 1000000"); goto fail;
        }
        load->stream_chunk_rows = (size_t)chunk;
        if (!parser_expect(parser, TOKEN_KW_FILAS, "Se esperaba 'filas' después del tamaño del lote")) goto fail;
    }
    /* Las opciones son parte del contrato AST, no texto interpretado por el backend. */
    while (parser_is_identifier(parser) && strcmp(parser->current.lexeme, "con") == 0) {
        parser_advance(parser);
        if (parser_match(parser, TOKEN_KW_REGISTROS)) {
            parser_advance(parser);
            if (!parser_expect_word(parser, "de", "Se esperaba 'de'")) goto fail;
            if (!parser_expect(parser, TOKEN_KW_HASTA, "Se esperaba 'hasta'")) goto fail;
            if (!parser_expect(parser, TOKEN_NUMERO, "El límite del registro debe ser numérico")) goto fail;
            double limit = parser->previous.number_value;
            if (!isfinite(limit) || limit < 0.004 || limit > 64.0 || floor(limit * 1024.0) != limit * 1024.0) {
                parser_error(parser, "El límite debe estar entre 0.004 y 64 MiB"); goto fail;
            }
            if (parser_match(parser, TOKEN_KW_MIB)) parser_advance(parser);
            else { parser_error(parser, "Se esperaba la unidad 'MiB'"); goto fail; }
            load->stream_record_limit = (size_t)(limit * 1024.0 * 1024.0);
        } else if (parser_is_identifier(parser) && strcmp(parser->current.lexeme, "grupos") == 0) {
            parser_advance(parser);
            if (!parser_expect_word(parser, "de", "Se esperaba 'de' después de grupos")) goto fail;
            if (!parser_expect(parser, TOKEN_NUMERO, "El límite de grupos debe ser numérico")) goto fail;
            double groups = parser->previous.number_value;
            if (!isfinite(groups) || groups < 1.0 || groups > 100000.0 || floor(groups) != groups) {
                parser_error(parser, "El límite de grupos debe ser un entero entre 1 y 100000"); goto fail;
            }
            load->stream_group_limit = (size_t)groups;
        } else if (parser_is_identifier(parser) && strcmp(parser->current.lexeme, "columnas") == 0) {
            parser_advance(parser);
            if (!parser_expect_word(parser, "de", "Se esperaba 'de' después de columnas")) goto fail;
            if (!parser_expect(parser, TOKEN_NUMERO, "El límite de columnas debe ser numérico")) goto fail;
            double columns = parser->previous.number_value;
            if (!isfinite(columns) || columns < 1.0 || columns > 4096.0 || floor(columns) != columns) {
                parser_error(parser, "El límite de columnas debe ser un entero entre 1 y 4096"); goto fail;
            }
            load->stream_column_limit = (size_t)columns;
        } else if (parser_match(parser, TOKEN_KW_FILAS)) {
            parser_advance(parser);
            if (!parser_expect(parser, TOKEN_KW_HASTA, "Se esperaba 'hasta' después de filas")) goto fail;
            if (!parser_expect(parser, TOKEN_NUMERO, "El límite de filas debe ser numérico")) goto fail;
            double rows = parser->previous.number_value;
            if (!isfinite(rows) || rows < 1.0 || rows > 1000000000.0 || floor(rows) != rows) {
                parser_error(parser, "El límite de filas debe ser un entero entre 1 y 1000000000"); goto fail;
            }
            load->stream_row_limit = (size_t)rows;
        } else if (parser_is_identifier(parser) && strcmp(parser->current.lexeme, "tiempo") == 0) {
            parser_advance(parser);
            if (!parser_expect(parser, TOKEN_KW_HASTA, "Se esperaba 'hasta' después de tiempo")) goto fail;
            if (!parser_expect(parser, TOKEN_NUMERO, "El presupuesto de tiempo debe ser numérico")) goto fail;
            double milliseconds = parser->previous.number_value;
            if (!isfinite(milliseconds) || milliseconds < 1.0 || milliseconds > 3600000.0 || floor(milliseconds) != milliseconds) {
                parser_error(parser, "El límite de tiempo debe ser un entero de 1 a 3600000 ms"); goto fail;
            }
            if (!parser_expect_word(parser, "ms", "Se esperaba la unidad 'ms'")) goto fail;
            load->stream_time_limit_ms = milliseconds;
        } else {
            parser_error(parser, "Se esperaba 'registros', 'grupos', 'columnas', 'filas' o 'tiempo' después de 'con'"); goto fail;
        }
    }
    if (parser_match(parser, TOKEN_PUNTO_Y_COMA)) parser_advance(parser);
    return load;
fail:
    ast_destroy(load);
    return NULL;
}

static ASTNode *parse_human_stream_export(Parser *parser) {
    parser_advance(parser);
    if (!parser_expect_word(parser, "resultado", "Se esperaba 'resultado'")) return NULL;
    if (!parser_expect_word(parser, "en", "Se esperaba 'en'")) return NULL;
    if (!parser_expect(parser, TOKEN_CADENA, "Se esperaba la ruta de salida entre comillas")) return NULL;
    ASTNode *export_node = ast_create_leaf(AST_BLOQUE_EXPORTAR, parser->previous.lexeme);
    if (!export_node) parser_error(parser, "Sin memoria para guardar resultado");
    if (parser_match(parser, TOKEN_PUNTO_Y_COMA)) parser_advance(parser);
    return export_node;
}

static ASTNode* parse_bloque_analisis(Parser *parser) {
    if (!parser_expect(parser, TOKEN_PUNTO, "Se esperaba '.'")) return NULL;
    if (!parser_expect(parser, TOKEN_KW_ANALISIS, "Se esperaba 'analisis'")) return NULL;
    if (!parser_expect(parser, TOKEN_IDENTIFICADOR, "Se esperaba nombre")) return NULL;
    
    ASTNode *node = ast_create_leaf(AST_BLOQUE_ANALISIS, parser->previous.lexeme);
    if (!node) return NULL;
    
    if (!parser_expect(parser, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
        ast_destroy(node);
        return NULL;
    }
    
    milena_symbols_enter_scope(&parser->symbols);
    // Parsear contenido del bloque
    while (!parser_match(parser, TOKEN_LLAVE_DER) &&
           !parser_match(parser, TOKEN_EOF) && !parser->has_error) {
        if (parser_match(parser, TOKEN_KW_DATOS)) {
            ASTNode *load = parse_human_stream_load(parser);
            if (load && !parser_add_child(parser, node, load,
                                           "Sin memoria para cargar datos")) break;
        } else if (parser_match(parser, TOKEN_KW_AGRUPAR)) {
            ASTNode *group = parse_stream_group(parser);
            if (group && !parser_add_child(parser, node, group,
                                           "Sin memoria para agrupación de flujo")) break;
        } else if (parser_match(parser, TOKEN_KW_RESUMIR)) {
            ASTNode *summary = parse_stream_summary(parser);
            if (summary && !parser_add_child(parser, node, summary,
                                               "Sin memoria para bloque resumir")) break;
        } else if (parser_match(parser, TOKEN_KW_GUARDAR)) {
            ASTNode *export_node = parse_human_stream_export(parser);
            if (export_node && !parser_add_child(parser, node, export_node,
                                                  "Sin memoria para guardar resultado")) break;
        } else if (parser_match(parser, TOKEN_KW_VARIABLE)) {
            ASTNode *declaration = parse_variable_declaration(parser);
            if (declaration && !parser_add_child(parser, node, declaration,
                                                   "Sin memoria para el AST")) break;
        } else if (parser_is_identifier(parser) &&
                   (strcmp(parser->current.lexeme, "entrada") == 0 ||
                    strcmp(parser->current.lexeme, "salida") == 0)) {
            bool is_output = strcmp(parser->current.lexeme, "salida") == 0;
            parser_advance(parser);
            if (!parser_is_identifier(parser)) {
                parser_error(parser, "Se esperaba tipo de rol de esquema");
                continue;
            }
            char type_name[MAX_TOKEN_LEN];
            strncpy(type_name, parser->current.lexeme, sizeof(type_name) - 1);
            type_name[sizeof(type_name) - 1] = '\0';
            if (is_output && strcmp(type_name, "binaria") != 0) {
                parser_error(parser, "La salida del esquema debe ser binaria");
                continue;
            }
            if (!is_output && strcmp(type_name, "categorica") != 0) {
                parser_error(parser, "La entrada del esquema debe ser categorica");
                continue;
            }
            parser_advance(parser);
            if (!parser_expect(parser, TOKEN_CADENA,
                               "Se esperaba nombre de columna entre comillas")) continue;
            char column_name[MAX_TOKEN_LEN];
            strncpy(column_name, parser->previous.lexeme, sizeof(column_name) - 1);
            column_name[sizeof(column_name) - 1] = '\0';
            ASTNode *role = ast_create_leaf(is_output ? AST_DECLARACION_SALIDA
                                                       : AST_DECLARACION_ENTRADA,
                                            column_name);
            if (!role) {
                parser_error(parser, "No se pudo crear el rol del esquema");
                continue;
            }
            role->type_name = milena_strdup(type_name);
            if (!role->type_name) {
                ast_destroy(role);
                parser_error(parser, "Sin memoria para el tipo del esquema");
                continue;
            }
            if (parser_match(parser, TOKEN_PUNTO_Y_COMA)) parser_advance(parser);
            if (!parser_add_child(parser, node, role, "Sin memoria para el AST")) break;
        } else if (parser_is_identifier(parser) &&
                   strcmp(parser->current.lexeme, "array") != 0 &&
                   strcmp(parser->current.lexeme, "arreglo") != 0) {
            ASTNode *assignment = parse_assignment(parser);
            if (assignment && !parser_add_child(parser, node, assignment,
                                                   "Sin memoria para el AST")) break;
        } else if (parser_is_identifier(parser) &&
            (strcmp(parser->current.lexeme, "array") == 0 ||
             strcmp(parser->current.lexeme, "arreglo") == 0)) {
            ASTNode *declaration = parse_array_declaration(parser);
            if (declaration && !parser_add_child(parser, node, declaration,
                                                   "Sin memoria para el AST")) break;
        } else if (parser_is_statistical_token(parser->current.type)) {
            ASTNode *statistic = parse_statistical_call(parser, true);
            if (statistic) {
                if (!parser_expect(parser, TOKEN_PUNTO_Y_COMA,
                                   "Se esperaba ';' después de la operación estadística")) {
                    ast_destroy(statistic);
                } else {
                    if (!parser_add_child(parser, node, statistic,
                                           "Sin memoria para el AST")) break;
                }
            }
        } else if (parser_match(parser, TOKEN_NUMERAL)) {
            parser_advance(parser);
            if (parser_match(parser, TOKEN_KW_DATOS)) {
                parser_advance(parser);
                parser_add_child(parser, node, ast_create(AST_DECLARACION_DATOS), "Sin memoria para declaración de datos");
            } else if (parser_match(parser, TOKEN_KW_ESTADISTICA)) {
                parser_advance(parser);
                parser_add_child(parser, node, ast_create(AST_DECLARACION_ESTADISTICA), "Sin memoria para declaración estadística");
            } else if (parser_is_identifier(parser) &&
                       (strcmp(parser->current.lexeme, "perfil_avanzado") == 0 ||
                        strcmp(parser->current.lexeme, "histograma") == 0 ||
                        strcmp(parser->current.lexeme, "normalidad") == 0 ||
                        strcmp(parser->current.lexeme, "tasa") == 0 ||
                        strcmp(parser->current.lexeme, "poisson") == 0 ||
                        strcmp(parser->current.lexeme, "correlacion") == 0 ||
                        strcmp(parser->current.lexeme, "wilcoxon") == 0 ||
                        strcmp(parser->current.lexeme, "chi_cuadrado") == 0 ||
                        strcmp(parser->current.lexeme, "riesgo") == 0 ||
                        strcmp(parser->current.lexeme, "modelo_sst") == 0 ||
                        strcmp(parser->current.lexeme, "interes_simple") == 0)) {
                char command_name[MAX_TOKEN_LEN];
                strncpy(command_name, parser->current.lexeme, sizeof(command_name) - 1);
                command_name[sizeof(command_name) - 1] = '\0';
                parser_advance(parser);
                if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                    if (parser_expect(parser, TOKEN_CADENA, "Se esperaba columna SST")) {
                        ASTNode *sst = ast_create_leaf(AST_COMANDO_SST,
                                                       parser->previous.lexeme);
                        if (sst) sst->type_name = milena_strdup(command_name);
                        if (sst && !parser_add_child(parser, node, sst, "Sin memoria para comando SST")) break;
                        parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')' después de SST");
                    }
                }
            }
        } else if (parser_match(parser, TOKEN_KW_DATASET)) {
            parser_advance(parser);
            if (parser_match(parser, TOKEN_KW_CARGAR)) {
                parser_advance(parser);
                if (parser_match(parser, TOKEN_KW_DATOS)) {
                    parser_advance(parser);
                }
                bool streaming = false;
                if (parser_is_identifier(parser) &&
                    strcmp(parser->current.lexeme, "flujo") == 0) {
                    streaming = true;
                    parser_advance(parser);
                }
                if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('") ) {
                    if (parser_expect(parser, TOKEN_CADENA, "Se esperaba archivo")) {
                        ASTNode *cargar = ast_create_leaf(AST_LLAMADA_CARGAR, parser->previous.lexeme);
                        if (streaming && cargar) {
                            cargar->type_name = milena_strdup("flujo");
                            if (!cargar->type_name) {
                                ast_destroy(cargar);
                                cargar = NULL;
                                parser_error(parser, "Sin memoria para modo flujo");
                            }
                        }
                        if (streaming && !parser_match(parser, TOKEN_PAR_DER)) {
                            if (!parser_expect(parser, TOKEN_COMA,
                                               "El modo flujo espera el tamaño del lote")) {
                                ast_destroy(cargar);
                                cargar = NULL;
                            } else if (!parser_expect(parser, TOKEN_NUMERO,
                                                      "El tamaño del lote debe ser numérico")) {
                                ast_destroy(cargar);
                                cargar = NULL;
                            } else if (cargar) {
                                double chunk = parser->previous.number_value;
                                if (!isfinite(chunk) || chunk < 1.0 || chunk > 1000000.0 ||
                                    floor(chunk) != chunk) {
                                    ast_destroy(cargar);
                                    cargar = NULL;
                                    parser_error(parser, "El tamaño del lote debe ser un entero entre 1 y 1000000");
                                } else {
                                    cargar->number_value = chunk;
                                }
                            }
                        }
                        if (cargar && !parser_add_child(parser, node, cargar,
                                                        "Sin memoria para cargar")) break;
                        parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')'");
                    }
                }
            }
        } else if (parser_match(parser, TOKEN_PUNTO)) {
            parser_advance(parser);
            if (parser_match(parser, TOKEN_KW_LIMPIAR)) {
                parser_advance(parser);
                if (parser_match(parser, TOKEN_KW_DATASET)) parser_advance(parser);
                if (parser_expect(parser, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
                    ASTNode *limpiar = ast_create(AST_BLOQUE_LIMPIAR);
                    while (!parser_match(parser, TOKEN_LLAVE_DER) &&
                           !parser_match(parser, TOKEN_EOF) && !parser->has_error) {
                        if (parser_match(parser, TOKEN_NUMERAL)) {
                            parser_advance(parser);
                            if (parser_match(parser, TOKEN_KW_NULOS)) {
                                parser_advance(parser);
                                if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                    if (parser_expect(parser, TOKEN_CADENA, "Se esperaba cadena")) {
                                        if (!parser_add_child(parser, limpiar, ast_create_leaf(AST_COMANDO_NULOS, parser->previous.lexeme), "Sin memoria para comando nulos")) break;
                                        parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')'");
                                    }
                                }
                            } else if (parser_match(parser, TOKEN_KW_DUPLICADOS)) {
                                parser_advance(parser);
                                if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                    if (parser_expect(parser, TOKEN_CADENA, "Se esperaba cadena")) {
                                        if (!parser_add_child(parser, limpiar, ast_create_leaf(AST_COMANDO_DUPLICADOS, parser->previous.lexeme), "Sin memoria para comando duplicados")) break;
                                        parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')'");
                                    }
                                }
                            }
                        } else {
                            parser_error(parser, "Comando desconocido en limpiar");
                        }
                    }
                    parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'");
                    if (limpiar) parser_add_child(parser, node, limpiar,
                                                   "Sin memoria para el AST");
                }
            } else if (parser_match(parser, TOKEN_KW_TRANSFORMAR)) {
                parser_advance(parser);
                if (parser_match(parser, TOKEN_KW_DATASET)) parser_advance(parser);
                if (parser_expect(parser, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
                    ASTNode *transformar = ast_create(AST_BLOQUE_TRANSFORMAR);
                    while (!parser_match(parser, TOKEN_LLAVE_DER) &&
                           !parser_match(parser, TOKEN_EOF) && !parser->has_error) {
                        if (parser_match(parser, TOKEN_NUMERAL)) {
                            parser_advance(parser);
                            if (parser_match(parser, TOKEN_KW_TOTAL)) {
                                parser_advance(parser);
                                if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                    if (parser_expect(parser, TOKEN_CADENA, "Se esperaba cadena")) {
                                        if (!parser_add_child(parser, transformar, ast_create_leaf(AST_COMANDO_TOTAL, parser->previous.lexeme), "Sin memoria para comando total")) break;
                                        parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')'");
                                    }
                                }
                            } else if (parser_match(parser, TOKEN_KW_PERIODO)) {
                                parser_advance(parser);
                                /* Accept both canonical forms: #periodo("...")
                                 * and the older #periodo extraer("..."). */
                                if (parser_match(parser, TOKEN_KW_EXTRAER)) parser_advance(parser);
                                if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('") ) {
                                    if (parser_expect(parser, TOKEN_CADENA, "Se esperaba cadena")) {
                                        if (!parser_add_child(parser, transformar, ast_create_leaf(AST_COMANDO_PERIODO, parser->previous.lexeme), "Sin memoria para comando periodo")) break;
                                        parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')'" );
                                    }
                                }
                            }
                        } else {
                            parser_error(parser, "Comando desconocido en transformar");
                        }
                    }
                    parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'");
                    if (transformar) parser_add_child(parser, node, transformar,
                                                       "Sin memoria para el AST");
                }
            } else if (parser_match(parser, TOKEN_KW_AGRUPAR)) {
                parser_advance(parser);
                if (parser_match(parser, TOKEN_KW_DATASET)) parser_advance(parser);
                if (parser_expect(parser, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
                    ASTNode *agrupar = ast_create(AST_BLOQUE_AGRUPAR);
                    while (!parser_match(parser, TOKEN_LLAVE_DER) &&
                           !parser_match(parser, TOKEN_EOF) && !parser->has_error) {
                        if (!parser_match(parser, TOKEN_NUMERAL)) {
                            parser_advance(parser);
                            continue;
                        }
                        parser_advance(parser);
                        if (parser_match(parser, TOKEN_KW_POR) ||
                            (parser_is_identifier(parser) &&
                             strcmp(parser->current.lexeme, "por") == 0)) {
                            parser_advance(parser);
                            if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                if (parser_expect(parser, TOKEN_CADENA, "Se esperaba columna de agrupación")) {
                                    if (agrupar && !parser_add_child(parser, agrupar,
                                        ast_create_leaf(AST_AGRUPACION_POR,
                                                        parser->previous.lexeme),
                                        "Sin memoria para agrupación")) break;
                                    parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')' después de por");
                                }
                            }
                        } else if (parser_match(parser, TOKEN_FUNCION_SUMA) ||
                                   parser_match(parser, TOKEN_FUNCION_MEDIA) ||
                                   parser_match(parser, TOKEN_FUNCION_MINIMO) ||
                                   parser_match(parser, TOKEN_FUNCION_MAXIMO)) {
                            char metric[MAX_TOKEN_LEN];
                            strncpy(metric, parser->current.lexeme, sizeof(metric) - 1);
                            metric[sizeof(metric) - 1] = '\0';
                            parser_advance(parser);
                            if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                if (parser_expect(parser, TOKEN_CADENA, "Se esperaba columna de resumen")) {
                                    char specification[MAX_TOKEN_LEN * 2];
                                    (void)snprintf(specification, sizeof(specification),
                                                   "%s:%s", metric, parser->previous.lexeme);
                                    if (agrupar && !parser_add_child(parser, agrupar,
                                        ast_create_leaf(AST_RESUMEN_METRICA, specification),
                                        "Sin memoria para métrica de agrupación")) break;
                                    parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')' después del resumen");
                                }
                            }
                        } else if (parser_match(parser, TOKEN_IDENTIFICADOR) &&
                                   strcmp(parser->current.lexeme, "conteo") == 0) {
                            parser_advance(parser);
                            if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                if (parser_expect(parser, TOKEN_CADENA, "Se esperaba columna de conteo")) {
                                    char specification[MAX_TOKEN_LEN * 2];
                                    (void)snprintf(specification, sizeof(specification),
                                                   "conteo:%s", parser->previous.lexeme);
                                    if (agrupar && !parser_add_child(parser, agrupar,
                                        ast_create_leaf(AST_RESUMEN_METRICA, specification),
                                        "Sin memoria para métrica de agrupación")) break;
                                    parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')' después del conteo");
                                }
                            }
                        } else {
                            parser_error(parser, "Comando desconocido en agrupar");
                        }
                    }
                    parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'");
                    if (agrupar && agrupar->child_count > 1) {
                        if (!parser_add_child(parser, node, agrupar, "Sin memoria para bloque agrupar")) break;
                    }
                    else ast_destroy(agrupar);
                }
            } else if (parser_match(parser, TOKEN_KW_RESUMIR)) {
                parser_advance(parser);
                if (parser_match(parser, TOKEN_KW_DATASET)) parser_advance(parser);
                if (parser_expect(parser, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
                    ASTNode *resumir = ast_create(AST_BLOQUE_RESUMIR);
                    while (!parser_match(parser, TOKEN_LLAVE_DER) &&
                           !parser_match(parser, TOKEN_EOF) && !parser->has_error) {
                        if (!parser_match(parser, TOKEN_NUMERAL)) {
                            parser_advance(parser);
                            continue;
                        }
                        parser_advance(parser);
                        char metric_buffer[MAX_TOKEN_LEN] = {0};
                        const char *metric = NULL;
                        if (parser_match(parser, TOKEN_FUNCION_SUMA) ||
                            parser_match(parser, TOKEN_FUNCION_MEDIA) ||
                            parser_match(parser, TOKEN_FUNCION_MINIMO) ||
                            parser_match(parser, TOKEN_FUNCION_MAXIMO) ||
                            parser_match(parser, TOKEN_FUNCION_VARIANZA) ||
                            parser_match(parser, TOKEN_FUNCION_DESVIACION) ||
                            parser_match(parser, TOKEN_FUNCION_MEDIANA) ||
                            parser_match(parser, TOKEN_FUNCION_PERCENTIL)) {
                            strncpy(metric_buffer, parser->current.lexeme, sizeof(metric_buffer) - 1);
                            metric = metric_buffer;
                        } else if (parser_match(parser, TOKEN_IDENTIFICADOR) &&
                                   strcmp(parser->current.lexeme, "conteo") == 0) {
                            metric = "conteo";
                        }
                        if (!metric) {
                            parser_error(parser, "Métrica desconocida en resumir");
                            break;
                        }
                        parser_advance(parser);
                        if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                            if (parser_expect(parser, TOKEN_CADENA, "Se esperaba columna de resumen")) {
                                char specification[MAX_TOKEN_LEN * 2];
                                (void)snprintf(specification, sizeof(specification),
                                               "%s:%s", metric, parser->previous.lexeme);
                                if (resumir && !parser_add_child(parser, resumir,
                                    ast_create_leaf(AST_RESUMEN_METRICA, specification),
                                    "Sin memoria para métrica de resumen")) break;
                                parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')' después del resumen");
                            }
                        }
                    }
                    parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'");
                    if (resumir && resumir->child_count > 0) {
                        if (!parser_add_child(parser, node, resumir, "Sin memoria para bloque resumir")) break;
                    }
                    else ast_destroy(resumir);
                }
            } else if (parser_match(parser, TOKEN_KW_EXPORTAR)) {
                parser_advance(parser);
                if (parser_expect(parser, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
                    ASTNode *exportar = NULL;
                    while (!parser_match(parser, TOKEN_LLAVE_DER) &&
                           !parser_match(parser, TOKEN_EOF) && !parser->has_error) {
                        if (parser_match(parser, TOKEN_PAR_IZQ)) {
                            parser_advance(parser);
                            if (!parser_expect(parser, TOKEN_CADENA,
                                               "Se esperaba archivo de exportación")) break;
                            exportar = ast_create_leaf(AST_BLOQUE_EXPORTAR,
                                                       parser->previous.lexeme);
                            if (!exportar) {
                                parser_error(parser, "Sin memoria para exportar");
                                break;
                            }
                            if (!parser_expect(parser, TOKEN_PAR_DER,
                                               "Se esperaba ')' después del archivo")) {
                                ast_destroy(exportar);
                                exportar = NULL;
                                break;
                            }
                        } else {
                            parser_advance(parser);
                        }
                    }
                    if (!parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'")) {
                        ast_destroy(exportar);
                    } else if (exportar) {
                        if (!parser_add_child(parser, node, exportar,
                                               "Sin memoria para el AST")) break;
                    }
                }
            } else {
                /* Los bloques nombrados de análisis conservan la condición
                 * en el AST; no se ejecutan mediante clasificación textual. */
                if (parser_is_identifier(parser)) {
                    char named_block[MAX_TOKEN_LEN];
                    strncpy(named_block, parser->current.lexeme, sizeof(named_block) - 1);
                    named_block[sizeof(named_block) - 1] = '\0';
                    bool selecting = strcmp(named_block, "seleccionar") == 0;
                    bool joining = strcmp(named_block, "unir") == 0;
                    parser_advance(parser);
                    if (parser_match(parser, TOKEN_LLAVE_IZQ)) {
                        parser_advance(parser);
                        ASTNode *filtrar = ast_create(selecting ? AST_BLOQUE_SELECCIONAR :
                                                       (joining ? AST_BLOQUE_UNIR : AST_BLOQUE_FILTRAR));
                        while (!parser_match(parser, TOKEN_LLAVE_DER) &&
                               !parser_match(parser, TOKEN_EOF) && !parser->has_error) {
                            if (parser_match(parser, TOKEN_KW_DATASET) ||
                                parser_match(parser, TOKEN_COMA)) {
                                parser_advance(parser);
                            } else if (parser_match(parser, TOKEN_PAR_IZQ)) {
                                parser_advance(parser);
                                if (parser_match(parser, TOKEN_KW_FILTRAR)) parser_advance(parser);
                                parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')' después de filtrar");
                            } else if (parser_match(parser, TOKEN_NUMERAL)) {
                                parser_advance(parser);
                                if (selecting && parser_is_identifier(parser) &&
                                    strcmp(parser->current.lexeme, "columnas") == 0) {
                                    parser_advance(parser);
                                    if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                        if (parser_expect(parser, TOKEN_CADENA, "Se esperaba lista de columnas")) {
                                            ASTNode *columns = ast_create_leaf(
                                                AST_COMANDO_COLUMNAS,
                                                parser->previous.lexeme);
                                            if (columns && filtrar && !parser_add_child(parser, filtrar, columns, "Sin memoria para columnas")) break;
                                            parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')' después de columnas");
                                        }
                                    }
                                } else if (joining && parser_is_identifier(parser) &&
                                           (strcmp(parser->current.lexeme, "derecha") == 0 ||
                                            strcmp(parser->current.lexeme, "clave") == 0)) {
                                    ASTNodeType command_type = strcmp(parser->current.lexeme, "derecha") == 0
                                        ? AST_COMANDO_DERECHA : AST_COMANDO_CLAVE;
                                    parser_advance(parser);
                                    if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                        if (parser_expect(parser, TOKEN_CADENA, "Se esperaba valor de unión")) {
                                            ASTNode *command = ast_create_leaf(command_type,
                                                                                parser->previous.lexeme);
                                            if (command && filtrar && !parser_add_child(parser, filtrar, command, "Sin memoria para unión")) break;
                                            parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')' después de unión");
                                        }
                                    }
                                } else if (parser_match(parser, TOKEN_KW_CONDICION)) {
                                    parser_advance(parser);
                                    if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                        if (parser_expect(parser, TOKEN_CADENA, "Se esperaba condición")) {
                                            ASTNode *condition = ast_create_leaf(
                                                AST_COMANDO_CONDICION,
                                                parser->previous.lexeme);
                                            if (condition && filtrar && !parser_add_child(parser, filtrar, condition, "Sin memoria para condición")) break;
                                            parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')' después de condición");
                                        }
                                    }
                                }
                            } else {
                                parser_advance(parser);
                            }
                        }
                        parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'");
                        if (filtrar && filtrar->child_count > 0) {
                            if (!parser_add_child(parser, node, filtrar, "Sin memoria para bloque de filtro")) break;
                        }
                        else ast_destroy(filtrar);
                    }
                }
            }
        } else {
            parser_error(parser, "Token inesperado en el bloque de análisis");
        }
    }
    
    if (!parser->has_error) {
        parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'");
    }
    milena_symbols_leave_scope(&parser->symbols);
    return node;
}


/* User-defined numeric functions. Expressions use numeric booleans (0/1). */
static ASTNode *fn_expr(Parser *p);
static int fn_precedence(TokenType t) {
    switch (t) {
    case TOKEN_IGUAL_IGUAL: case TOKEN_DISTINTO: case TOKEN_MAYOR:
    case TOKEN_MAYOR_IGUAL: case TOKEN_MENOR: case TOKEN_MENOR_IGUAL: return 1;
    case TOKEN_MAS: case TOKEN_MENOS: return 2;
    case TOKEN_POR: case TOKEN_DIV: return 3;
    default: return 0;
    }
}
static const char *fn_operator(TokenType t) {
    switch (t) { case TOKEN_IGUAL_IGUAL:return "=="; case TOKEN_DISTINTO:return "!=";
    case TOKEN_MAYOR:return ">"; case TOKEN_MAYOR_IGUAL:return ">="; case TOKEN_MENOR:return "<";
    case TOKEN_MENOR_IGUAL:return "<="; case TOKEN_MAS:return "+"; case TOKEN_MENOS:return "-";
    case TOKEN_POR:return "*"; default:return "/"; }
}
static ASTNode *fn_primary(Parser *p) {
    ASTNode *n = NULL;
    if (p->current.type == TOKEN_NUMERO) {
        double value = p->current.number_value;
        parser_advance(p);
        n = ast_create_number(value);
    } else if (p->current.type == TOKEN_BOOLEANO) {
        bool yes = strcmp(p->current.lexeme, "verdadero") == 0;
        parser_advance(p);
        n = ast_create_number(yes ? 1.0 : 0.0);
    } else if (p->current.type == TOKEN_PAR_IZQ) {
        parser_advance(p);
        n = fn_expr(p);
        if (!n || !parser_expect(p, TOKEN_PAR_DER, "Se esperaba ')'")) {
            ast_destroy(n);
            return NULL;
        }
    } else if (parser_is_identifier(p)) {
        char name[MAX_TOKEN_LEN];
        strncpy(name, p->current.lexeme, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        parser_advance(p);
        if (p->current.type == TOKEN_PAR_IZQ) {
            parser_advance(p);
            n = ast_create_leaf(AST_EXPRESION_LLAMADA, name);
            if (!n) {
                parser_error(p, "Sin memoria para llamada");
                return NULL;
            }
            if (!parser_match(p, TOKEN_PAR_DER)) {
                for (;;) {
                    ASTNode *arg = fn_expr(p);
                    if (!arg || !parser_add_child(p, n, arg,
                                                   "Sin memoria para argumento")) {
                        ast_destroy(n);
                        return NULL;
                    }
                    if (parser_match(p, TOKEN_PAR_DER)) break;
                    if (!parser_expect(p, TOKEN_COMA, "Se esperaba ','")) {
                        ast_destroy(n);
                        return NULL;
                    }
                }
            }
            parser_advance(p);
        } else {
            n = ast_create_leaf(AST_EXPRESION_IDENTIFICADOR, name);
        }
    } else {
        parser_error(p, "Se esperaba expresión numérica o booleana");
        return NULL;
    }
    if (!n) parser_error(p, "Sin memoria para expresión");
    return n;
}

static ASTNode *fn_expr_prec(Parser *p, int min_prec) {
    ASTNode *left = fn_primary(p);
    if (!left) return NULL;
    for (;;) {
        int prec = fn_precedence(p->current.type);
        if (prec < min_prec) break;
        TokenType token = p->current.type;
        parser_advance(p);
        ASTNode *right = fn_expr_prec(p, prec + 1);
        if (!right) { ast_destroy(left); return NULL; }
        ASTNode *op = ast_create_leaf(AST_EXPRESION_OPERACION, fn_operator(token));
        if (!op) {
            ast_destroy(left); ast_destroy(right);
            parser_error(p, "Sin memoria para operación");
            return NULL;
        }
        if (!parser_add_child(p, op, left, "Sin memoria para operando izquierdo") ||
            !parser_add_child(p, op, right, "Sin memoria para operando derecho")) {
            ast_destroy(op);
            return NULL;
        }
        left = op;
    }
    return left;
}

static ASTNode *fn_expr(Parser *p) { return fn_expr_prec(p, 1); }

static ASTNode *parse_fn_body(Parser *p) {
    ASTNode *body = ast_create(AST_BLOQUE_FUNCION);
    if (!body) { parser_error(p, "Sin memoria para cuerpo de función"); return NULL; }
    while (!parser_match(p, TOKEN_LLAVE_DER) && !parser_match(p, TOKEN_EOF)) {
        ASTNode *statement = NULL;
        if (p->current.type == TOKEN_KW_RETORNAR) {
            parser_advance(p);
            ASTNode *expression = fn_expr(p);
            if (!expression || !parser_expect(p, TOKEN_PUNTO_Y_COMA,
                                               "Se esperaba ';' después de retornar")) {
                ast_destroy(expression); ast_destroy(body); return NULL;
            }
            statement = ast_create(AST_COMANDO_RETORNAR);
            if (!statement || !parser_add_child(p, statement, expression,
                                                  "Sin memoria para retorno")) {
                ast_destroy(statement); ast_destroy(body); return NULL;
            }
        } else if (p->current.type == TOKEN_KW_SI) {
            parser_advance(p);
            if (!parser_expect(p, TOKEN_PAR_IZQ, "Se esperaba '(' después de si")) {
                ast_destroy(body); return NULL;
            }
            ASTNode *condition = fn_expr(p);
            if (!condition || !parser_expect(p, TOKEN_PAR_DER, "Se esperaba ')'")) {
                ast_destroy(condition); ast_destroy(body); return NULL;
            }
            if (!parser_expect(p, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
                ast_destroy(condition); ast_destroy(body); return NULL;
            }
            statement = ast_create(AST_CONDICION_SI);
            if (!statement || !parser_add_child(p, statement, condition,
                                                  "Sin memoria para condición")) {
                ast_destroy(statement); ast_destroy(body); return NULL;
            }
            ASTNode *then_body = parse_fn_body(p);
            if (!then_body) { ast_destroy(statement); ast_destroy(body); return NULL; }
            size_t count = then_body->child_count;
            for (size_t k = 0; k < count; ++k) {
                ASTNode *child = then_body->children[k];
                then_body->children[k] = NULL;
                if (!parser_add_child(p, statement, child,
                                      "Sin memoria para cuerpo condicional")) {
                    then_body->child_count = 0;
                    ast_destroy(then_body); ast_destroy(statement); ast_destroy(body);
                    return NULL;
                }
            }
            then_body->child_count = 0;
            ast_destroy(then_body);
            if (!parser_expect(p, TOKEN_LLAVE_DER, "Se esperaba '}'")) {
                ast_destroy(statement); ast_destroy(body); return NULL;
            }
            if (p->current.type == TOKEN_KW_SINO) {
                parser_advance(p);
                if (!parser_expect(p, TOKEN_LLAVE_IZQ, "Se esperaba '{' después de sino")) {
                    ast_destroy(statement); ast_destroy(body); return NULL;
                }
                ASTNode *else_body = parse_fn_body(p);
                if (!else_body) { ast_destroy(statement); ast_destroy(body); return NULL; }
                ASTNode *else_block = ast_create(AST_BLOQUE_FUNCION);
                if (!else_block) {
                    ast_destroy(else_body); ast_destroy(statement); ast_destroy(body);
                    parser_error(p, "Sin memoria para bloque sino"); return NULL;
                }
                count = else_body->child_count;
                for (size_t k = 0; k < count; ++k) {
                    ASTNode *child = else_body->children[k];
                    else_body->children[k] = NULL;
                    if (!parser_add_child(p, else_block, child,
                                          "Sin memoria para cuerpo sino")) {
                        else_body->child_count = 0;
                        ast_destroy(else_body); ast_destroy(else_block);
                        ast_destroy(statement); ast_destroy(body); return NULL;
                    }
                }
                else_body->child_count = 0;
                ast_destroy(else_body);
                if (!parser_add_child(p, statement, else_block,
                                      "Sin memoria para bloque sino") ||
                    !parser_expect(p, TOKEN_LLAVE_DER, "Se esperaba '}' después de sino")) {
                    ast_destroy(statement); ast_destroy(body); return NULL;
                }
            }
        } else if (p->current.type == TOKEN_KW_VARIABLE || parser_is_identifier(p)) {
            bool declaration = p->current.type == TOKEN_KW_VARIABLE;
            if (declaration) parser_advance(p);
            if (!parser_is_identifier(p)) {
                parser_error(p, "Se esperaba nombre"); ast_destroy(body); return NULL;
            }
            char name[MAX_TOKEN_LEN];
            strncpy(name, p->current.lexeme, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            parser_advance(p);
            if (!parser_expect(p, TOKEN_IGUAL, "Se esperaba '='")) {
                ast_destroy(body); return NULL;
            }
            ASTNode *expression = fn_expr(p);
            if (!expression || !parser_expect(p, TOKEN_PUNTO_Y_COMA,
                                               "Se esperaba ';'")) {
                ast_destroy(expression); ast_destroy(body); return NULL;
            }
            statement = ast_create_leaf(declaration ? AST_DECLARACION_VARIABLE :
                                        AST_ASIGNACION_VARIABLE, name);
            if (!statement || !parser_add_child(p, statement, expression,
                                                  "Sin memoria para expresión de variable")) {
                ast_destroy(statement); ast_destroy(body); return NULL;
            }
        } else {
            parser_error(p, "Sentencia no válida en función");
            ast_destroy(body); return NULL;
        }
        if (!parser_add_child(p, body, statement, "Sin memoria para sentencia")) {
            ast_destroy(body); return NULL;
        }
    }
    return body;
}

static ASTNode *parse_user_function(Parser *p) {
    parser_advance(p);
    if (!parser_expect(p, TOKEN_IDENTIFICADOR, "Se esperaba nombre de función")) return NULL;
    char name[MAX_TOKEN_LEN];
    strncpy(name, p->previous.lexeme, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    if (!parser_expect(p, TOKEN_PAR_IZQ, "Se esperaba '('") ) return NULL;
    ASTNode *params = ast_create(AST_BLOQUE_FUNCION);
    if (!params) { parser_error(p, "Sin memoria para parámetros"); return NULL; }
    if (!parser_match(p, TOKEN_PAR_DER)) {
        for (;;) {
            if (!parser_is_identifier(p)) {
                parser_error(p, "Se esperaba parámetro"); ast_destroy(params); return NULL;
            }
            ASTNode *parameter = ast_create_leaf(AST_EXPRESION_IDENTIFICADOR,
                                                 p->current.lexeme);
            if (!parser_add_child(p, params, parameter, "Sin memoria para parámetro")) {
                ast_destroy(params); return NULL;
            }
            parser_advance(p);
            if (parser_match(p, TOKEN_PAR_DER)) break;
            if (!parser_expect(p, TOKEN_COMA, "Se esperaba ','")) {
                ast_destroy(params); return NULL;
            }
        }
    }
    parser_advance(p);
    if (!parser_expect(p, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
        ast_destroy(params); return NULL;
    }
    ASTNode *body = parse_fn_body(p);
    if (!body) { ast_destroy(params); return NULL; }
    if (!parser_expect(p, TOKEN_LLAVE_DER, "Se esperaba '}'")) {
        ast_destroy(params); ast_destroy(body); return NULL;
    }
    ASTNode *function = ast_create_leaf(AST_DECLARACION_FUNCION, name);
    if (!function || !parser_add_child(p, function, params, "Sin memoria para parámetros") ||
        !parser_add_child(p, function, body, "Sin memoria para cuerpo de función")) {
        ast_destroy(function); return NULL;
    }
    return function;
}

static ASTNode *parse_user_program(Parser *p) {
    ASTNode *program = ast_create(AST_PROGRAMA);
    if (!program) { parser_error(p, "Sin memoria para programa"); return NULL; }
    while (!parser_match(p, TOKEN_EOF)) {
        if (p->current.type == TOKEN_KW_FUNCION) {
            ASTNode *function = parse_user_function(p);
            if (!function || !parser_add_child(p, program, function,
                                                 "Sin memoria para función")) {
                ast_destroy(program); return NULL;
            }
        } else if (p->current.type == TOKEN_KW_VARIABLE) {
            ASTNode *body = parse_fn_body(p);
            if (!body) { ast_destroy(program); return NULL; }
            size_t count = body->child_count;
            for (size_t i = 0; i < count; ++i) {
                ASTNode *child = body->children[i];
                body->children[i] = NULL;
                if (!parser_add_child(p, program, child,
                                      "Sin memoria para sentencia global")) {
                    body->child_count = 0; ast_destroy(body); ast_destroy(program); return NULL;
                }
            }
            body->child_count = 0;
            ast_destroy(body);
        } else {
            parser_error(p, "Se esperaba función o variable");
            ast_destroy(program); return NULL;
        }
    }
    return program;
}

ASTNode* parser_parse(Parser *parser) {
    if (!parser || parser->has_error) return NULL;
    if (parser->current.type == TOKEN_KW_FUNCION) {
        return parse_user_program(parser);
    }

    ASTNode *program = ast_create(AST_PROGRAMA);
    if (!program) {
        parser_error(parser, "Error de memoria");
        return NULL;
    }

    if (parser_is_statistical_token(parser->current.type)) {
        while (parser_is_statistical_token(parser->current.type) &&
               !parser->has_error) {
            ASTNode *statistic = parse_statistical_call(parser, false);
            if (!statistic) break;
            if (!parser_add_child(parser, program, statistic,
                                  "Sin memoria para el AST")) break;
            if (parser_match(parser, TOKEN_PUNTO_Y_COMA)) parser_advance(parser);
        }
    } else {
        ASTNode *analysis = parse_bloque_analisis(parser);
        if (analysis && !parser_add_child(parser, program, analysis,
                                          "Sin memoria para el AST")) {
            ast_destroy(program);
            return NULL;
        }
    }

    if (!parser->has_error && !parser_match(parser, TOKEN_EOF)) {
        parser_error(parser, "Se esperaba fin de archivo");
    }
    if (parser->has_error) {
        ast_destroy(program);
        return NULL;
    }
    return program;
}

ASTNode* parser_parse_statistical_call(Parser *parser) {
    return parse_statistical_call(parser, false);
}
