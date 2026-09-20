#include "parser.h"

void parser_init(Parser *parser, Lexer *lexer) {
    if (!parser) return;
    memset(parser, 0, sizeof(*parser));
    parser->lexer = lexer;
    milena_error_clear(&parser->error);
    if (lexer) {
        parser->current = lexer_next_token(lexer);
        parser->previous = parser->current;
    } else {
        parser_error(parser, "El lexer no puede ser nulo");
    }
}

void parser_error(Parser *parser, const char *msg) {
    if (!parser || parser->has_error) return;
    milena_error_set(&parser->error, MILENA_ERR_PARSE,
                     (size_t)(parser->current.line < 0 ? 0 : parser->current.line),
                     (size_t)(parser->current.column < 0 ? 0 : parser->current.column),
                     0, msg);
    parser->has_error = true;
}

void parser_advance(Parser *parser) {
    if (!parser || !parser->lexer) return;
    parser->previous = parser->current;
    parser->current = lexer_next_token(parser->lexer);
    if (parser->current.type == TOKEN_ERROR) {
        parser_error(parser, parser->current.lexeme);
    }
}

bool parser_match(Parser *parser, TokenType type) {
    return parser && parser->current.type == type;
}

bool parser_expect(Parser *parser, TokenType type, const char *msg) {
    if (!parser_match(parser, type)) {
        parser_error(parser, msg);
        return false;
    }
    parser_advance(parser);
    return !parser->has_error;
}

static bool parser_add_child(Parser *parser, ASTNode *parent, ASTNode *child) {
    if (!child) {
        parser_error(parser, "No se pudo reservar memoria para el AST");
        return false;
    }
    if (!ast_add_child(parent, child)) {
        ast_destroy(child);
        parser_error(parser, "No se pudo ampliar el AST");
        return false;
    }
    return true;
}

static bool is_array_word(const Token *token) {
    return token && token->type == TOKEN_IDENTIFICADOR &&
           (strcmp(token->lexeme, "array") == 0 ||
            strcmp(token->lexeme, "arreglo") == 0);
}

static bool is_statistical_token(TokenType type) {
    return type >= TOKEN_FUNCION_SUMA && type <= TOKEN_FUNCION_PERCENTIL;
}

static ASTStatOperation statistical_operation(TokenType type) {
    switch (type) {
        case TOKEN_FUNCION_SUMA: return AST_ESTADISTICA_SUMA;
        case TOKEN_FUNCION_MEDIA: return AST_ESTADISTICA_MEDIA;
        case TOKEN_FUNCION_MINIMO: return AST_ESTADISTICA_MINIMO;
        case TOKEN_FUNCION_MAXIMO: return AST_ESTADISTICA_MAXIMO;
        case TOKEN_FUNCION_VARIANZA: return AST_ESTADISTICA_VARIANZA;
        case TOKEN_FUNCION_DESVIACION: return AST_ESTADISTICA_DESVIACION;
        case TOKEN_FUNCION_MEDIANA: return AST_ESTADISTICA_MEDIANA;
        case TOKEN_FUNCION_PERCENTIL: return AST_ESTADISTICA_PERCENTIL;
        default: return AST_STAT_OPERATION_COUNT;
    }
}

static bool array_symbol_exists(const ASTNode *analysis, const char *name) {
    if (!analysis || !name) return false;
    for (size_t i = 0; i < analysis->child_count; i++) {
        const ASTNode *child = analysis->children[i];
        if (child && child->type == AST_DECLARACION_ARRAY && child->value &&
            strcmp(child->value, name) == 0) return true;
    }
    return false;
}

static ASTNode *parse_array_declaration(Parser *parser) {
    if (!parser || !is_array_word(&parser->current)) return NULL;
    int line = parser->current.line;
    int column = parser->current.column;
    parser_advance(parser);
    if (!parser_expect(parser, TOKEN_IDENTIFICADOR,
                       "Se esperaba el nombre del arreglo")) return NULL;

    char name[MAX_TOKEN_LEN];
    memcpy(name, parser->previous.lexeme, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    if (!parser_expect(parser, TOKEN_IGUAL,
                       "Se esperaba '=' en la declaración del arreglo") ||
        !parser_expect(parser, TOKEN_CORCHETE_IZQ,
                       "Se esperaba '[' en el literal del arreglo")) return NULL;
    if (parser_match(parser, TOKEN_CORCHETE_DER)) {
        parser_error(parser, "Un literal de arreglo no puede estar vacío");
        return NULL;
    }

    ASTNode *literal = ast_create(AST_EXPRESION_ARRAY);
    if (!literal) {
        parser_error(parser, "No se pudo crear el literal del arreglo");
        return NULL;
    }
    while (!parser->has_error) {
        if (!parser_expect(parser, TOKEN_NUMERO,
                           "El literal de arreglo solo admite números")) break;
        ASTNode *number = ast_create_number(parser->previous.number_value);
        if (!parser_add_child(parser, literal, number)) break;
        if (parser_match(parser, TOKEN_CORCHETE_DER)) {
            parser_advance(parser);
            break;
        }
        if (!parser_expect(parser, TOKEN_COMA,
                           "Se esperaba ',' entre los elementos del arreglo")) break;
        if (parser_match(parser, TOKEN_CORCHETE_DER)) {
            parser_error(parser, "No se admite una coma final en el arreglo");
            break;
        }
    }
    if (parser->has_error ||
        !parser_expect(parser, TOKEN_PUNTO_Y_COMA,
                       "Se esperaba ';' después del arreglo")) {
        ast_destroy(literal);
        return NULL;
    }

    ASTNode *declaration = ast_create_leaf(AST_DECLARACION_ARRAY, name);
    if (!declaration) {
        ast_destroy(literal);
        parser_error(parser, "No se pudo crear la declaración del arreglo");
        return NULL;
    }
    declaration->line = line;
    declaration->column = column;
    if (!parser_add_child(parser, declaration, literal)) {
        ast_destroy(declaration);
        return NULL;
    }
    return declaration;
}

static ASTNode *parse_statistical_call(Parser *parser, const ASTNode *analysis) {
    if (!parser || !is_statistical_token(parser->current.type)) return NULL;
    ASTStatOperation operation = statistical_operation(parser->current.type);
    int line = parser->current.line;
    int column = parser->current.column;
    parser_advance(parser);
    if (!parser_expect(parser, TOKEN_PAR_IZQ,
                       "Se esperaba '(' después de la operación estadística") ||
        !parser_expect(parser, TOKEN_IDENTIFICADOR,
                       "Se esperaba un símbolo de arreglo")) return NULL;

    char symbol[MAX_TOKEN_LEN];
    memcpy(symbol, parser->previous.lexeme, sizeof(symbol));
    symbol[sizeof(symbol) - 1] = '\0';
    if (!array_symbol_exists(analysis, symbol)) {
        parser_error(parser, "El símbolo de arreglo no ha sido declarado antes de usarlo");
        return NULL;
    }

    double percentile = 0.0;
    if (operation == AST_ESTADISTICA_PERCENTIL) {
        if (!parser_expect(parser, TOKEN_COMA,
                           "percentil requiere un valor entre 0 y 100") ||
            !parser_expect(parser, TOKEN_NUMERO,
                           "Se esperaba un percentil numérico")) return NULL;
        percentile = parser->previous.number_value;
        if (!isfinite(percentile) || percentile < 0.0 || percentile > 100.0) {
            parser_error(parser, "El percentil debe estar entre 0 y 100");
            return NULL;
        }
    }

    int axis = -1;
    bool axis_seen = false;
    bool keepdims = false;
    bool keepdims_seen = false;
    while (parser_match(parser, TOKEN_COMA) && !parser->has_error) {
        parser_advance(parser);
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
                parser_error(parser, "La opción de conservar dimensiones solo puede indicarse una vez");
                break;
            }
            keepdims_seen = true;
            bool negate = parser_match(parser, TOKEN_CONCEPTO_SIN);
            parser_advance(parser);
            if (negate && !parser_expect(parser, TOKEN_CONCEPTO_CONSERVAR,
                                         "Se esperaba 'conservar' después de 'sin'")) break;
            if (!parser_expect(parser, TOKEN_CONCEPTO_DIMENSIONES,
                               "Se esperaba 'dimensiones' en la opción keepdims")) break;
            keepdims = !negate;
        } else {
            parser_error(parser, "Opción estadística inesperada");
            break;
        }
    }
    if (parser->has_error) return NULL;
    if (!parser_expect(parser, TOKEN_PAR_DER,
                       "Se esperaba ')' al final de la operación estadística")) return NULL;
    if (keepdims_seen && !axis_seen) {
        parser_error(parser, "Conservar dimensiones requiere especificar un eje");
        return NULL;
    }

    ASTNode *argument = ast_create_leaf(AST_EXPRESION_IDENTIFICADOR, symbol);
    if (!argument) {
        parser_error(parser, "No se pudo crear el símbolo estadístico");
        return NULL;
    }
    ASTNode *node = ast_create_statistic(operation, argument, axis, keepdims, percentile);
    if (!node) {
        parser_error(parser, "No se pudo crear la operación estadística");
        return NULL;
    }
    node->line = line;
    node->column = column;
    return node;
}

static bool skip_balanced_block(Parser *parser) {
    if (!parser_expect(parser, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) return false;
    unsigned depth = 1;
    while (depth > 0 && !parser_match(parser, TOKEN_EOF) && !parser->has_error) {
        if (parser_match(parser, TOKEN_LLAVE_IZQ)) depth++;
        else if (parser_match(parser, TOKEN_LLAVE_DER)) depth--;
        parser_advance(parser);
    }
    if (depth != 0 && !parser->has_error) {
        parser_error(parser, "Se esperaba '}'");
        return false;
    }
    return !parser->has_error;
}

static bool parse_legacy_item(Parser *parser, ASTNode *analysis) {
    if (parser_match(parser, TOKEN_NUMERAL)) {
        parser_advance(parser);
        ASTNodeType type;
        if (parser_match(parser, TOKEN_KW_DATOS)) type = AST_DECLARACION_DATOS;
        else if (parser_match(parser, TOKEN_KW_ESTADISTICA)) type = AST_DECLARACION_ESTADISTICA;
        else {
            parser_error(parser, "Se esperaba 'datos' o 'estadistica' después de '#'");
            return false;
        }
        parser_advance(parser);
        return parser_add_child(parser, analysis, ast_create(type));
    }
    if (parser_match(parser, TOKEN_KW_DATASET)) {
        parser_advance(parser);
        if (!parser_expect(parser, TOKEN_KW_CARGAR, "Se esperaba 'cargar'") ) return false;
        if (parser_match(parser, TOKEN_KW_DATOS)) parser_advance(parser);
        if (!parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('") ||
            !parser_expect(parser, TOKEN_CADENA, "Se esperaba el archivo") ) return false;
        ASTNode *load = ast_create_leaf(AST_LLAMADA_CARGAR, parser->previous.lexeme);
        if (!parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')'") ) {
            ast_destroy(load);
            return false;
        }
        if (parser_match(parser, TOKEN_PUNTO_Y_COMA)) parser_advance(parser);
        return parser_add_child(parser, analysis, load);
    }
    if (parser_match(parser, TOKEN_PUNTO)) {
        parser_advance(parser);
        ASTNodeType type = AST_BLOQUE_EXPORTAR;
        if (parser_match(parser, TOKEN_KW_LIMPIAR)) type = AST_BLOQUE_LIMPIAR;
        else if (parser_match(parser, TOKEN_KW_TRANSFORMAR)) type = AST_BLOQUE_TRANSFORMAR;
        else if (parser_match(parser, TOKEN_KW_FILTRAR)) type = AST_BLOQUE_FILTRAR;
        else if (parser_match(parser, TOKEN_KW_AGRUPAR)) type = AST_BLOQUE_AGRUPAR;
        else if (parser_match(parser, TOKEN_KW_RESUMIR)) type = AST_BLOQUE_RESUMIR;
        else if (parser_match(parser, TOKEN_KW_VISUALIZAR)) type = AST_BLOQUE_VISUALIZAR;
        else if (!parser_match(parser, TOKEN_KW_EXPORTAR) &&
                 !parser_match(parser, TOKEN_IDENTIFICADOR)) {
            parser_error(parser, "Se esperaba el nombre de un bloque");
            return false;
        }
        parser_advance(parser);
        if (parser_match(parser, TOKEN_KW_DATASET)) parser_advance(parser);
        ASTNode *block = ast_create(type);
        if (!block) {
            parser_error(parser, "No se pudo crear el bloque");
            return false;
        }
        if (!skip_balanced_block(parser)) {
            ast_destroy(block);
            return false;
        }
        return parser_add_child(parser, analysis, block);
    }
    parser_error(parser, "Elemento no reconocido dentro del bloque de análisis");
    return false;
}

static ASTNode *parse_analysis(Parser *parser) {
    if (!parser_expect(parser, TOKEN_PUNTO, "Se esperaba '.'") ||
        !parser_expect(parser, TOKEN_KW_ANALISIS, "Se esperaba 'analisis'") ||
        !parser_expect(parser, TOKEN_IDENTIFICADOR, "Se esperaba el nombre del análisis")) {
        return NULL;
    }
    ASTNode *analysis = ast_create_leaf(AST_BLOQUE_ANALISIS, parser->previous.lexeme);
    if (!analysis) {
        parser_error(parser, "No se pudo crear el bloque de análisis");
        return NULL;
    }
    if (!parser_expect(parser, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
        ast_destroy(analysis);
        return NULL;
    }

    while (!parser_match(parser, TOKEN_LLAVE_DER) &&
           !parser_match(parser, TOKEN_EOF) && !parser->has_error) {
        size_t before = parser->lexer ? parser->lexer->position : 0;
        ASTNode *item = NULL;
        if (is_array_word(&parser->current)) {
            item = parse_array_declaration(parser);
        } else if (is_statistical_token(parser->current.type)) {
            item = parse_statistical_call(parser, analysis);
            if (item && !parser_expect(parser, TOKEN_PUNTO_Y_COMA,
                                       "Se esperaba ';' después de la operación estadística")) {
                ast_destroy(item);
                item = NULL;
            }
        } else {
            if (!parse_legacy_item(parser, analysis)) break;
        }
        if (item && !parser_add_child(parser, analysis, item)) break;
        if (!parser->has_error && parser->lexer && parser->lexer->position == before) {
            parser_error(parser, "El parser no avanzó al procesar el elemento");
        }
    }
    if (parser->has_error ||
        !parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'")) {
        ast_destroy(analysis);
        return NULL;
    }
    return analysis;
}

ASTNode *parser_parse(Parser *parser) {
    if (!parser || parser->has_error) return NULL;
    ASTNode *program = ast_create(AST_PROGRAMA);
    if (!program) {
        parser_error(parser, "No se pudo crear el programa");
        return NULL;
    }
    ASTNode *analysis = parse_analysis(parser);
    if (!analysis || !parser_add_child(parser, program, analysis)) {
        ast_destroy(program);
        return NULL;
    }
    if (!parser_match(parser, TOKEN_EOF)) {
        parser_error(parser, "Se esperaba el fin del archivo");
        ast_destroy(program);
        return NULL;
    }
    return program;
}
