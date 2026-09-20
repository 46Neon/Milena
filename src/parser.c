#include "parser.h"

void parser_init(Parser *parser, Lexer *lexer) {
    parser->lexer = lexer;
    parser->current = lexer_next_token(lexer);
    parser->previous = parser->current;
    parser->has_error = false;
    milena_error_init(&parser->error);
    milena_symbols_init(&parser->symbols);
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
    /* Names remain usable when the lexer classifies a reserved word as a
       keyword.  `total` is part of the language vocabulary but is also a
       valid variable name in existing scripts. */
    /* Keyword classification must not make the established variable name
       `total` unusable.  Check the spelling first so this remains true even
       if the lexer token classification changes. */
    if (parser->current.lexeme[0] != '\0' &&
        strcmp(parser->current.lexeme, "total") == 0) return true;
    return parser_match(parser, TOKEN_IDENTIFICADOR) ||
           parser_match(parser, TOKEN_KW_TOTAL);
}

static bool parser_match_lexeme(Parser *parser, TokenType type, const char *lexeme) {
    return parser_match(parser, type) ||
           (parser_match(parser, TOKEN_IDENTIFICADOR) &&
            strcmp(parser->current.lexeme, lexeme) == 0);
}

bool parser_expect(Parser *parser, TokenType type, const char *msg) {
    if (!parser_match(parser, type)) {
        parser_error(parser, msg);
        return false;
    }
    parser_advance(parser);
    return true;
}

static ASTNode *parse_array_declaration(Parser *parser) {
    if (!parser || !parser_is_identifier(parser) ||
        (strcmp(parser->current.lexeme, "array") != 0 &&
         strcmp(parser->current.lexeme, "arreglo") != 0)) return NULL;
    parser_advance(parser);

    if (!parser_expect(parser, TOKEN_IDENTIFICADOR,
                       "Se esperaba nombre del array")) return NULL;
    char name[MAX_TOKEN_LEN];
    strncpy(name, parser->previous.lexeme, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    if (!parser_expect(parser, TOKEN_IGUAL, "Se esperaba '=' en la declaración del array") ||
        !parser_expect(parser, TOKEN_CORCHETE_IZQ, "Se esperaba '[' en el literal del array")) {
        return NULL;
    }
    if (parser_match(parser, TOKEN_CORCHETE_DER)) {
        parser_error(parser, "Un literal de array no puede estar vacío");
        return NULL;
    }

    ASTNode *array = ast_create(AST_EXPRESION_ARRAY);
    if (!array) {
        parser_error(parser, "No se pudo crear el literal del array");
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
        ast_add_child(array, number);
        if (parser_match(parser, TOKEN_CORCHETE_DER)) break;
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
    parser_advance(parser);
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
    ast_add_child(declaration, array);
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
        ast_add_child(operation, left);
        ast_add_child(operation, right);
        left = operation;
    }
    return left;
}

static ASTNode *parse_variable_declaration(Parser *parser) {
    parser_advance(parser);
    if (!parser_is_identifier(parser)) {
        parser_error(parser, "Se esperaba nombre de variable");
        return NULL;
    }
    char name[MAX_TOKEN_LEN];
    strncpy(name, parser->current.lexeme, sizeof(name) - 1); name[sizeof(name) - 1] = '\0';
    parser_advance(parser);
    if (!parser_expect(parser, TOKEN_IGUAL, "Se esperaba '=' en la declaración de variable")) return NULL;
    /* Register the name before parsing its initializer so the declaration
       participates in the same scope rules as subsequent expressions. */
    if (milena_symbols_declare(&parser->symbols, name, &parser->error) != MILENA_OK) {
        parser->has_error = true;
        return NULL;
    }
    ASTNode *value = parse_expression(parser);
    if (!value) return NULL;
    ASTNode *node = ast_create_leaf(AST_DECLARACION_VARIABLE, name);
    if (!node) {
        ast_destroy(value);
        parser_error(parser, "No se pudo crear la variable");
        return NULL;
    }
    ast_add_child(node, value);
    if (!parser_expect(parser, TOKEN_PUNTO_Y_COMA, "Se esperaba ';' después de la variable")) {
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
    ast_add_child(node, value);
    if (!parser_expect(parser, TOKEN_PUNTO_Y_COMA, "Se esperaba ';' después de la asignación")) {
        ast_destroy(node);
        return NULL;
    }
    return node;
}

static ASTNode* parse_bloque_analisis(Parser *parser) {
    if (!parser_expect(parser, TOKEN_PUNTO, "Se esperaba '.'")) return NULL;
    if (!parser_match_lexeme(parser, TOKEN_KW_ANALISIS, "analisis")) {
        parser_error(parser, "Se esperaba 'analisis'");
        return NULL;
    }
    parser_advance(parser);
    if (!parser_expect(parser, TOKEN_IDENTIFICADOR, "Se esperaba nombre")) return NULL;
    
    ASTNode *node = ast_create_leaf(AST_BLOQUE_ANALISIS, parser->previous.lexeme);
    if (!node) return NULL;
    
    if (!parser_expect(parser, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
        ast_destroy(node);
        return NULL;
    }
    
    milena_symbols_enter_scope(&parser->symbols);
    // Parsear contenido del bloque
    while (!parser_match(parser, TOKEN_LLAVE_DER) && !parser_match(parser, TOKEN_EOF)) {
        if (parser_match_lexeme(parser, TOKEN_KW_VARIABLE, "variable")) {
            ASTNode *declaration = parse_variable_declaration(parser);
            if (declaration) ast_add_child(node, declaration);
        } else if (parser_is_identifier(parser) &&
                   strcmp(parser->current.lexeme, "array") != 0 &&
                   strcmp(parser->current.lexeme, "arreglo") != 0) {
            ASTNode *assignment = parse_assignment(parser);
            if (assignment) ast_add_child(node, assignment);
        } else if (parser_is_identifier(parser) &&
            (strcmp(parser->current.lexeme, "array") == 0 ||
             strcmp(parser->current.lexeme, "arreglo") == 0)) {
            ASTNode *declaration = parse_array_declaration(parser);
            if (declaration) ast_add_child(node, declaration);
        } else if (parser_match(parser, TOKEN_NUMERAL)) {
            parser_advance(parser);
            if (parser_match(parser, TOKEN_KW_DATOS)) {
                parser_advance(parser);
                ast_add_child(node, ast_create(AST_DECLARACION_DATOS));
            } else if (parser_match(parser, TOKEN_KW_ESTADISTICA)) {
                parser_advance(parser);
                ast_add_child(node, ast_create(AST_DECLARACION_ESTADISTICA));
            }
        } else if (parser_match(parser, TOKEN_KW_DATASET)) {
            parser_advance(parser);
            if (parser_match(parser, TOKEN_KW_CARGAR)) {
                parser_advance(parser);
                if (parser_match(parser, TOKEN_KW_DATOS)) {
                    parser_advance(parser);
                }
                if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                    if (parser_expect(parser, TOKEN_CADENA, "Se esperaba archivo")) {
                        ASTNode *cargar = ast_create_leaf(AST_LLAMADA_CARGAR, parser->previous.lexeme);
                        if (cargar) ast_add_child(node, cargar);
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
                    while (!parser_match(parser, TOKEN_LLAVE_DER) && !parser_match(parser, TOKEN_EOF)) {
                        if (parser_match(parser, TOKEN_NUMERAL)) {
                            parser_advance(parser);
                            if (parser_match(parser, TOKEN_KW_NULOS)) {
                                parser_advance(parser);
                                if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                    if (parser_expect(parser, TOKEN_CADENA, "Se esperaba cadena")) {
                                        ast_add_child(limpiar, ast_create_leaf(AST_COMANDO_NULOS, parser->previous.lexeme));
                                        parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')'");
                                    }
                                }
                            } else if (parser_match(parser, TOKEN_KW_DUPLICADOS)) {
                                parser_advance(parser);
                                if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                    if (parser_expect(parser, TOKEN_CADENA, "Se esperaba cadena")) {
                                        ast_add_child(limpiar, ast_create_leaf(AST_COMANDO_DUPLICADOS, parser->previous.lexeme));
                                        parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')'");
                                    }
                                }
                            }
                        }
                    }
                    parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'");
                    ast_add_child(node, limpiar);
                }
            } else if (parser_match(parser, TOKEN_KW_TRANSFORMAR)) {
                parser_advance(parser);
                if (parser_match(parser, TOKEN_KW_DATASET)) parser_advance(parser);
                if (parser_expect(parser, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
                    ASTNode *transformar = ast_create(AST_BLOQUE_TRANSFORMAR);
                    while (!parser_match(parser, TOKEN_LLAVE_DER) && !parser_match(parser, TOKEN_EOF)) {
                        if (parser_match(parser, TOKEN_NUMERAL)) {
                            parser_advance(parser);
                            if (parser_match(parser, TOKEN_KW_TOTAL)) {
                                parser_advance(parser);
                                if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                    if (parser_expect(parser, TOKEN_CADENA, "Se esperaba cadena")) {
                                        ast_add_child(transformar, ast_create_leaf(AST_COMANDO_TOTAL, parser->previous.lexeme));
                                        parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')'");
                                    }
                                }
                            } else if (parser_match(parser, TOKEN_KW_PERIODO)) {
                                parser_advance(parser);
                                if (parser_match(parser, TOKEN_KW_EXTRAER)) {
                                    parser_advance(parser);
                                    if (parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '('")) {
                                        if (parser_expect(parser, TOKEN_CADENA, "Se esperaba cadena")) {
                                            ast_add_child(transformar, ast_create_leaf(AST_COMANDO_PERIODO, parser->previous.lexeme));
                                            parser_expect(parser, TOKEN_PAR_DER, "Se esperaba ')'");
                                        }
                                    }
                                }
                            }
                        }
                    }
                    parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'");
                    ast_add_child(node, transformar);
                }
            } else if (parser_match(parser, TOKEN_KW_EXPORTAR)) {
                parser_advance(parser);
                if (parser_expect(parser, TOKEN_LLAVE_IZQ, "Se esperaba '{'")) {
                    // Saltar contenido
                    while (!parser_match(parser, TOKEN_LLAVE_DER) && !parser_match(parser, TOKEN_EOF)) {
                        parser_advance(parser);
                    }
                    parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'");
                }
            } else {
                // Otros bloques
                if (parser_is_identifier(parser)) {
                    parser_advance(parser);
                    if (parser_match(parser, TOKEN_LLAVE_IZQ)) {
                        parser_advance(parser);
                        while (!parser_match(parser, TOKEN_LLAVE_DER) && !parser_match(parser, TOKEN_EOF)) {
                            parser_advance(parser);
                        }
                        parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'");
                    }
                }
            }
        } else {
            parser_advance(parser);
        }
    }
    
    parser_expect(parser, TOKEN_LLAVE_DER, "Se esperaba '}'");
    milena_symbols_leave_scope(&parser->symbols);
    return node;
}

ASTNode* parser_parse(Parser *parser) {
    ASTNode *program = ast_create(AST_PROGRAMA);
    if (!program) {
        parser_error(parser, "Error de memoria");
        return NULL;
    }
    
    while (!parser_match(parser, TOKEN_EOF) && !parser->has_error) {
        if (parser->current.type == TOKEN_FUNCION_MEDIANA ||
            parser->current.type == TOKEN_FUNCION_PERCENTIL) {
            ASTNode *statistic = parser_parse_statistical_call(parser);
            if (!statistic) break;
            ast_add_child(program, statistic);
            if (parser_match(parser, TOKEN_PUNTO_Y_COMA)) parser_advance(parser);
        } else {
            ASTNode *analisis = parse_bloque_analisis(parser);
            if (!analisis) break;
            ast_add_child(program, analisis);
        }
    }

    if (!parser->has_error && !parser_match(parser, TOKEN_EOF)) {
        parser_error(parser, "Se esperaba fin de archivo");
    }
    
    return program;
}

ASTNode* parser_parse_statistical_call(Parser *parser) {
    if (!parser) return NULL;
    bool median = parser->current.type == TOKEN_FUNCION_MEDIANA;
    bool percentile = parser->current.type == TOKEN_FUNCION_PERCENTIL;
    if (!median && !percentile) { parser_error(parser, "Se esperaba una operación estadística"); return NULL; }
    parser_advance(parser);
    if (!parser_expect(parser, TOKEN_PAR_IZQ, "Se esperaba '(' después de la operación")) return NULL;
    if (!parser_expect(parser, TOKEN_IDENTIFICADOR, "Se esperaba un arreglo como argumento")) return NULL;
    ASTNode *argument = ast_create_leaf(AST_EXPRESION_IDENTIFICADOR, parser->previous.lexeme);
    if (!argument) { parser_error(parser, "No se pudo crear el argumento estadístico"); return NULL; }
    double percentile_value = 50.0; int axis = -1; bool keepdims = false;
    if (percentile) {
        if (!parser_expect(parser, TOKEN_COMA, "Se esperaba el porcentaje" ) ||
            !parser_expect(parser, TOKEN_NUMERO, "Se esperaba un porcentaje numérico")) { ast_destroy(argument); return NULL; }
        percentile_value = parser->previous.number_value;
        if (percentile_value < 0.0 || percentile_value > 100.0) { ast_destroy(argument); parser_error(parser, "El porcentaje debe estar entre 0 y 100"); return NULL; }
    }
    while (!parser_match(parser, TOKEN_PAR_DER)) {
        if (!parser_expect(parser, TOKEN_COMA, "Se esperaba ',' entre argumentos")) { ast_destroy(argument); return NULL; }
        if (parser_match(parser, TOKEN_CONCEPTO_EJE)) {
            parser_advance(parser);
            if (!parser_expect(parser, TOKEN_NUMERO, "Se esperaba un número después de 'eje'")) { ast_destroy(argument); return NULL; }
            axis = (int)parser->previous.number_value;
        } else if (parser_match(parser, TOKEN_CONCEPTO_CONSERVAR)) {
            parser_advance(parser);
            if (!parser_expect(parser, TOKEN_FUNCION_DIMENSIONES, "Se esperaba 'dimensiones' después de 'conservar'")) { ast_destroy(argument); return NULL; }
            keepdims = true;
        } else { ast_destroy(argument); parser_error(parser, "Argumento estadístico inesperado"); return NULL; }
    }
    parser_advance(parser);
    ASTStatOperation op = median ? AST_ESTADISTICA_MEDIANA : AST_ESTADISTICA_PERCENTIL;
    return ast_create_statistic(op, argument, axis, keepdims, percentile_value);
}
