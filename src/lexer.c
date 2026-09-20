#include "lexer.h"

static char lexer_current(Lexer *lexer) {
    if (lexer->position >= lexer->length) return '\0';
    return lexer->source[lexer->position];
}

static char lexer_peek_char(Lexer *lexer, int offset) {
    size_t pos = lexer->position + offset;
    if (pos >= lexer->length) return '\0';
    return lexer->source[pos];
}

static char lexer_advance_char(Lexer *lexer) {
    char c = lexer_current(lexer);
    if (c != '\0') {
        lexer->position++;
        if (c == '\n') {
            lexer->line++;
            lexer->column = 1;
        } else {
            lexer->column++;
        }
    }
    return c;
}

static bool is_identifier_start(char c) {
    return isalpha(c) || c == '_' || c >= 0x80;
}

static bool is_identifier_char(char c) {
    return isalnum(c) || c == '_' || c >= 0x80;
}

static bool is_keyword(const char *str) {
    static const char *keywords[] = {
        "analisis", "datos", "estadistica", "dataset", "limpiar",
        "transformar", "visualizar", "exportar", "filtrar", "agrupar",
        "resumir", "cargar", "nulos", "duplicados", "condicion",
        "extraer", "total", "periodo", "verdadero", "falso"
    };
    static const int num_keywords = 20;
    
    for (int i = 0; i < num_keywords; i++) {
        if (strcmp(str, keywords[i]) == 0) return true;
    }
    return false;
}

static TokenType keyword_type(const char *str) {
    if (strcmp(str, "analisis") == 0) return TOKEN_KW_ANALISIS;
    if (strcmp(str, "datos") == 0) return TOKEN_KW_DATOS;
    if (strcmp(str, "estadistica") == 0) return TOKEN_KW_ESTADISTICA;
    if (strcmp(str, "dataset") == 0) return TOKEN_KW_DATASET;
    if (strcmp(str, "limpiar") == 0) return TOKEN_KW_LIMPIAR;
    if (strcmp(str, "transformar") == 0) return TOKEN_KW_TRANSFORMAR;
    if (strcmp(str, "visualizar") == 0) return TOKEN_KW_VISUALIZAR;
    if (strcmp(str, "exportar") == 0) return TOKEN_KW_EXPORTAR;
    if (strcmp(str, "filtrar") == 0) return TOKEN_KW_FILTRAR;
    if (strcmp(str, "agrupar") == 0) return TOKEN_KW_AGRUPAR;
    if (strcmp(str, "resumir") == 0) return TOKEN_KW_RESUMIR;
    if (strcmp(str, "cargar") == 0) return TOKEN_KW_CARGAR;
    if (strcmp(str, "nulos") == 0) return TOKEN_KW_NULOS;
    if (strcmp(str, "duplicados") == 0) return TOKEN_KW_DUPLICADOS;
    if (strcmp(str, "condicion") == 0) return TOKEN_KW_CONDICION;
    if (strcmp(str, "extraer") == 0) return TOKEN_KW_EXTRAER;
    if (strcmp(str, "total") == 0) return TOKEN_KW_TOTAL;
    if (strcmp(str, "periodo") == 0) return TOKEN_KW_PERIODO;
    if (strcmp(str, "verdadero") == 0 || strcmp(str, "falso") == 0) return TOKEN_BOOLEANO;
    return TOKEN_IDENTIFICADOR;
}

static Token lexer_create_token(Lexer *lexer, TokenType type, const char *lexeme) {
    Token token;
    token.type = type;
    strncpy(token.lexeme, lexeme, MAX_TOKEN_LEN - 1);
    token.lexeme[MAX_TOKEN_LEN - 1] = '\0';
    token.line = lexer->line;
    token.column = lexer->column;
    token.number_value = 0.0;
    return token;
}

static void lexer_skip_whitespace_and_comments(Lexer *lexer) {
    bool done = false;
    while (!done) {
        done = true;
        
        while (isspace(lexer_current(lexer))) {
            lexer_advance_char(lexer);
            done = false;
        }
        
        if (lexer_current(lexer) == '/' && lexer_peek_char(lexer, 1) == '/') {
            while (lexer_current(lexer) != '\n' && lexer_current(lexer) != '\0') {
                lexer_advance_char(lexer);
            }
            done = false;
        }
    }
}

void lexer_init(Lexer *lexer, const char *source) {
    lexer->source = source ? source : "";
    lexer->position = 0;
    lexer->length = strlen(source);
    lexer->line = 1;
    lexer->column = 1;
    milena_error_init(&lexer->error);
    lexer->current_token.type = TOKEN_EOF;
    lexer->current_token.lexeme[0] = '\0';
    lexer->previous_token = lexer->current_token;
}

Token lexer_next_token(Lexer *lexer) {
    lexer->previous_token = lexer->current_token;
    lexer_skip_whitespace_and_comments(lexer);
    
    char c = lexer_current(lexer);
    Token token;
    token.type = TOKEN_ERROR;
    token.lexeme[0] = '\0';
    token.line = lexer->line;
    token.column = lexer->column;
    token.number_value = 0.0;
    
    if (c == '\0') {
        token.type = TOKEN_EOF;
        strcpy(token.lexeme, "EOF");
        lexer->current_token = token;
        return token;
    }
    
    // Puntuación simple
    if (c == '.') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_PUNTO, ".");
        lexer->current_token = token;
        return token;
    }
    
    if (c == '#') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_NUMERAL, "#");
        lexer->current_token = token;
        return token;
    }
    
    if (c == '{') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_LLAVE_IZQ, "{");
        lexer->current_token = token;
        return token;
    }
    
    if (c == '}') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_LLAVE_DER, "}");
        lexer->current_token = token;
        return token;
    }
    
    if (c == '(') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_PAR_IZQ, "(");
        lexer->current_token = token;
        return token;
    }
    
    if (c == ')') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_PAR_DER, ")");
        lexer->current_token = token;
        return token;
    }

    if (c == '[') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_CORCHETE_IZQ, "[");
        lexer->current_token = token;
        return token;
    }

    if (c == ']') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_CORCHETE_DER, "]");
        lexer->current_token = token;
        return token;
    }
    
    if (c == ':') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_DOS_PUNTOS, ":");
        lexer->current_token = token;
        return token;
    }
    
    if (c == ',') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_COMA, ",");
        lexer->current_token = token;
        return token;
    }
    
    if (c == ';') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_PUNTO_Y_COMA, ";");
        lexer->current_token = token;
        return token;
    }
    
    // Operadores
    if (c == '=') {
        lexer_advance_char(lexer);
        if (lexer_current(lexer) == '=') {
            lexer_advance_char(lexer);
            token = lexer_create_token(lexer, TOKEN_IGUAL_IGUAL, "==");
        } else {
            token = lexer_create_token(lexer, TOKEN_IGUAL, "=");
        }
        lexer->current_token = token;
        return token;
    }
    
    if (c == '>') {
        lexer_advance_char(lexer);
        if (lexer_current(lexer) == '=') {
            lexer_advance_char(lexer);
            token = lexer_create_token(lexer, TOKEN_MAYOR_IGUAL, ">=");
        } else {
            token = lexer_create_token(lexer, TOKEN_MAYOR, ">");
        }
        lexer->current_token = token;
        return token;
    }
    
    if (c == '<') {
        lexer_advance_char(lexer);
        if (lexer_current(lexer) == '=') {
            lexer_advance_char(lexer);
            token = lexer_create_token(lexer, TOKEN_MENOR_IGUAL, "<=");
        } else {
            token = lexer_create_token(lexer, TOKEN_MENOR, "<");
        }
        lexer->current_token = token;
        return token;
    }
    
    if (c == '+') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_MAS, "+");
        lexer->current_token = token;
        return token;
    }
    
    if (c == '-') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_MENOS, "-");
        lexer->current_token = token;
        return token;
    }
    
    if (c == '*') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_POR, "*");
        lexer->current_token = token;
        return token;
    }
    
    if (c == '/') {
        lexer_advance_char(lexer);
        token = lexer_create_token(lexer, TOKEN_DIV, "/");
        lexer->current_token = token;
        return token;
    }
    
    // Cadenas
    if (c == '"') {
        lexer_advance_char(lexer);
        char buffer[MAX_TOKEN_LEN];
        size_t idx = 0;
        
        while (lexer_current(lexer) != '\0' && lexer_current(lexer) != '"') {
            if (lexer_current(lexer) == '\\') {
                lexer_advance_char(lexer);
                char esc = lexer_current(lexer);
                if (esc == 'n') buffer[idx++] = '\n';
                else if (esc == 't') buffer[idx++] = '\t';
                else if (esc == 'r') buffer[idx++] = '\r';
                else if (esc == '\\') buffer[idx++] = '\\';
                else if (esc == '"') buffer[idx++] = '"';
                else buffer[idx++] = esc;
                lexer_advance_char(lexer);
            } else {
                buffer[idx++] = lexer_advance_char(lexer);
            }
        }
        
        if (lexer_current(lexer) != '"') {
            token = lexer_create_token(lexer, TOKEN_ERROR, "cadena sin cerrar");
        } else {
            lexer_advance_char(lexer);
            buffer[idx] = '\0';
            token = lexer_create_token(lexer, TOKEN_CADENA, buffer);
        }
        
        lexer->current_token = token;
        return token;
    }
    
    // Números
    if (isdigit(c) || (c == '-' && isdigit(lexer_peek_char(lexer, 1)))) {
        char buffer[MAX_TOKEN_LEN];
        size_t idx = 0;
        bool has_dot = false;
        
        if (c == '-') {
            buffer[idx++] = lexer_advance_char(lexer);
        }
        
        while (isdigit(lexer_current(lexer)) || 
               (lexer_current(lexer) == '.' && !has_dot && isdigit(lexer_peek_char(lexer, 1)))) {
            if (lexer_current(lexer) == '.') has_dot = true;
            buffer[idx++] = lexer_advance_char(lexer);
        }
        
        buffer[idx] = '\0';
        token = lexer_create_token(lexer, TOKEN_NUMERO, buffer);
        token.number_value = atof(buffer);
        lexer->current_token = token;
        return token;
    }
    
    // Identificadores y palabras clave
    if (is_identifier_start(c)) {
        char buffer[MAX_TOKEN_LEN];
        size_t idx = 0;
        
        while (is_identifier_char(lexer_current(lexer))) {
            buffer[idx++] = lexer_advance_char(lexer);
        }
        
        buffer[idx] = '\0';
        TokenType type = keyword_type(buffer);
        token = lexer_create_token(lexer, type, buffer);
        
        if (type == TOKEN_BOOLEANO) {
            token.number_value = strcmp(buffer, "verdadero") == 0 ? 1.0 : 0.0;
        }
        
        lexer->current_token = token;
        return token;
    }
    
    // Carácter desconocido
    token = lexer_create_token(lexer, TOKEN_ERROR, "carácter desconocido");
    char err_msg[64];
    snprintf(err_msg, sizeof(err_msg), "Carácter inesperado: '%c'", c);
    milena_error_set(&lexer->error, MILENA_ERROR_LEXICAL, err_msg, lexer->line, lexer->column);
    lexer_advance_char(lexer);
    lexer->current_token = token;
    return token;
}

Token lexer_peek_token(Lexer *lexer) {
    size_t old_pos = lexer->position;
    int old_line = lexer->line;
    int old_col = lexer->column;
    Token prev = lexer->previous_token;
    Token curr = lexer->current_token;
    
    Token token = lexer_next_token(lexer);
    
    lexer->position = old_pos;
    lexer->line = old_line;
    lexer->column = old_col;
    lexer->previous_token = prev;
    lexer->current_token = curr;
    
    return token;
}

void lexer_advance_token(Lexer *lexer) {
    lexer->current_token = lexer->previous_token;
}

bool lexer_match(Lexer *lexer, TokenType type) {
    return lexer->current_token.type == type;
}

bool lexer_expect(Lexer *lexer, TokenType type, const char *error_msg) {
    if (lexer->current_token.type != type) {
        milena_error_set(&lexer->error, MILENA_ERROR_SYNTAX, error_msg, 
                      lexer->current_token.line, lexer->current_token.column);
        return false;
    }
    return true;
}

const char *token_type_name(TokenType type) {
    static const char *names[] = {
        "EOF", "ERROR",
        "ANALISIS", "DATOS", "ESTADISTICA", "DATASET", "LIMPIAR",
        "TRANSFORMAR", "VISUALIZAR", "EXPORTAR", "FILTRAR", "AGRUPAR",
        "RESUMIR", "CARGAR", "NULOS", "DUPLICADOS", "CONDICION",
        "EXTRAER", "TOTAL", "PERIODO",
        "PUNTO", "NUMERAL", "LLAVE_IZQ", "LLAVE_DER", "PAR_IZQ", "PAR_DER",
        "CORCHETE_IZQ", "CORCHETE_DER", "DOS_PUNTOS", "COMA", "PUNTO_Y_COMA",
        "IGUAL", "IGUAL_IGUAL", "DISTINTO", "MAYOR", "MAYOR_IGUAL",
        "MENOR", "MENOR_IGUAL", "MAS", "MENOS", "POR", "DIV", "ASIGNACION",
        "IDENTIFICADOR", "CADENA", "NUMERO", "BOOLEANO", "COMMENT"
    };
    size_t count = sizeof(names) / sizeof(names[0]);
    if ((size_t)type >= count) return "DESCONOCIDO";
    return names[(size_t)type];
}

bool token_is_keyword(TokenType type) {
    return type >= TOKEN_KW_ANALISIS && type <= TOKEN_KW_PERIODO;
}

bool token_is_operator(TokenType type) {
    return (type >= TOKEN_IGUAL && type <= TOKEN_DIV) || 
           type == TOKEN_ASIGNACION;
}
