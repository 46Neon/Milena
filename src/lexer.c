#include "lexer.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>

static char lexer_current(Lexer *lexer) {
    if (lexer->position >= lexer->length) return '\0';
    return lexer->source[lexer->position];
}

static char lexer_peek_char(Lexer *lexer, size_t offset) {
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
    return isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

static bool is_identifier_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

static MilenaTokenType keyword_type(const char *str) {
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
    if (strcmp(str, "forma") == 0) return TOKEN_FUNCION_FORMA;
    if (strcmp(str, "dimensiones") == 0) return TOKEN_FUNCION_DIMENSIONES;
    if (strcmp(str, "tamaño") == 0) return TOKEN_FUNCION_TAMANO;
    if (strcmp(str, "suma") == 0) return TOKEN_FUNCION_SUMA;
    if (strcmp(str, "media") == 0) return TOKEN_FUNCION_MEDIA;
    if (strcmp(str, "minimo") == 0) return TOKEN_FUNCION_MINIMO;
    if (strcmp(str, "maximo") == 0) return TOKEN_FUNCION_MAXIMO;
    if (strcmp(str, "varianza") == 0) return TOKEN_FUNCION_VARIANZA;
    if (strcmp(str, "desviacion_estandar") == 0) return TOKEN_FUNCION_DESVIACION;
    if (strcmp(str, "mediana") == 0) return TOKEN_FUNCION_MEDIANA;
    if (strcmp(str, "percentil") == 0) return TOKEN_FUNCION_PERCENTIL;
    if (strcmp(str, "eje") == 0) return TOKEN_CONCEPTO_EJE;
    if (strcmp(str, "conservar") == 0) return TOKEN_CONCEPTO_CONSERVAR;
    if (strcmp(str, "sin") == 0) return TOKEN_CONCEPTO_SIN;
    if (strcmp(str, "variable") == 0) return TOKEN_KW_VARIABLE;
    if (strcmp(str, "funcion") == 0 || strcmp(str, "función") == 0) return TOKEN_KW_FUNCION;
    if (strcmp(str, "retornar") == 0) return TOKEN_KW_RETORNAR;
    if (strcmp(str, "si") == 0) return TOKEN_KW_SI;
    if (strcmp(str, "sino") == 0) return TOKEN_KW_SINO;
    if (strcmp(str, "desde") == 0) return TOKEN_KW_DESDE;
    if (strcmp(str, "procesar") == 0) return TOKEN_KW_PROCESAR;
    if (strcmp(str, "por") == 0) return TOKEN_KW_POR;
    if (strcmp(str, "lotes") == 0) return TOKEN_KW_LOTES;
    if (strcmp(str, "filas") == 0) return TOKEN_KW_FILAS;
    if (strcmp(str, "guardar") == 0) return TOKEN_KW_GUARDAR;
    if (strcmp(str, "registros") == 0) return TOKEN_KW_REGISTROS;
    if (strcmp(str, "hasta") == 0) return TOKEN_KW_HASTA;
    if (strcmp(str, "MiB") == 0) return TOKEN_KW_MIB;
    if (strcmp(str, "contar") == 0) return TOKEN_KW_CONTAR;
    if (strcmp(str, "verdadero") == 0 || strcmp(str, "falso") == 0) return TOKEN_BOOLEANO;
    return TOKEN_IDENTIFICADOR;
}

static Token lexer_create_token(Lexer *lexer, MilenaTokenType type, const char *lexeme) {
    Token token;
    token.type = type;
    strncpy(token.lexeme, lexeme, MAX_TOKEN_LEN - 1);
    token.lexeme[MAX_TOKEN_LEN - 1] = '\0';
    token.line = lexer->token_start_line;
    token.column = lexer->token_start_column;
    token.end_line = lexer->line;
    token.end_column = lexer->column;
    token.start_offset = lexer->token_start_offset;
    token.end_offset = lexer->position;
    token.number_value = 0.0;
    return token;
}

static void lexer_skip_whitespace_and_comments(Lexer *lexer) {
    bool done = false;
    while (!done) {
        done = true;
        
        while (isspace((unsigned char)lexer_current(lexer))) {
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
    lexer->length = strlen(lexer->source);
    lexer->line = 1;
    lexer->column = 1;
    lexer->token_start_offset = 0;
    lexer->token_start_line = 1;
    lexer->token_start_column = 1;
    milena_error_init(&lexer->error);
    memset(&lexer->current_token, 0, sizeof(lexer->current_token));
    lexer->current_token.type = TOKEN_EOF;
    strcpy(lexer->current_token.lexeme, "EOF");
    lexer->current_token.line = lexer->current_token.end_line = 1;
    lexer->current_token.column = lexer->current_token.end_column = 1;
    lexer->previous_token = lexer->current_token;
}

Token lexer_next_token(Lexer *lexer) {
    lexer->previous_token = lexer->current_token;
    lexer_skip_whitespace_and_comments(lexer);
    lexer->token_start_offset = lexer->position;
    lexer->token_start_line = lexer->line;
    lexer->token_start_column = lexer->column;
    
    char c = lexer_current(lexer);
    Token token;
    token.type = TOKEN_ERROR;
    token.lexeme[0] = '\0';
    token.line = lexer->token_start_line;
    token.column = lexer->token_start_column;
    token.end_line = lexer->line;
    token.end_column = lexer->column;
    token.start_offset = lexer->token_start_offset;
    token.end_offset = lexer->position;
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
    
    if (c == '!') {
        lexer_advance_char(lexer);
        if (lexer_current(lexer) == '=') { lexer_advance_char(lexer); token = lexer_create_token(lexer, TOKEN_DISTINTO, "!="); lexer->current_token = token; return token; }
        token = lexer_create_token(lexer, TOKEN_ERROR, "!"); lexer->current_token = token; return token;
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
        
        bool too_long = false;
        bool invalid_escape = false;
        bool closed = false;
        while (lexer_current(lexer) != '\0' && lexer_current(lexer) != '"') {
            char value;
            if (lexer_current(lexer) == '\\') {
                lexer_advance_char(lexer);
                char esc = lexer_current(lexer);
                if (esc == 'n') value = '\n';
                else if (esc == 't') value = '\t';
                else if (esc == 'r') value = '\r';
                else if (esc == '\\') value = '\\';
                else if (esc == '"') value = '"';
                else {
                    value = esc;
                    invalid_escape = true;
                }
                if (esc != '\0') lexer_advance_char(lexer);
            } else {
                value = lexer_advance_char(lexer);
            }
            if (idx + 1 < sizeof(buffer)) buffer[idx++] = value;
            else too_long = true;
        }
        if (lexer_current(lexer) == '"') {
            lexer_advance_char(lexer);
            closed = true;
        }
        if (too_long) {
            token = lexer_create_token(lexer, TOKEN_ERROR, "cadena demasiado larga");
            milena_error_set(&lexer->error, MILENA_ERR_PARSE,
                             token.line, token.column, 0,
                             "La cadena supera el límite de 255 caracteres");
            lexer->current_token = token;
            return token;
        }
        if (invalid_escape) {
            token = lexer_create_token(lexer, TOKEN_ERROR, "escape no válido");
            milena_error_set(&lexer->error, MILENA_ERR_PARSE,
                             token.line, token.column, 0,
                             "La cadena contiene una secuencia de escape no admitida");
        } else if (!closed) {
            token = lexer_create_token(lexer, TOKEN_ERROR, "cadena sin cerrar");
            milena_error_set(&lexer->error, MILENA_ERR_PARSE,
                             token.line, token.column, 0,
                             "Cadena sin cerrar");
        } else {
            buffer[idx] = '\0';
            token = lexer_create_token(lexer, TOKEN_CADENA, buffer);
        }
        
        lexer->current_token = token;
        return token;
    }
    
    // Números: decimal opcional y exponente decimal opcional. El signo inicial
    // siempre es un operador; sólo el exponente puede contener un signo.
    if (isdigit((unsigned char)c)) {
        char buffer[MAX_TOKEN_LEN];
        size_t idx = 0;
        bool has_dot = false;
        bool too_long = false;
        bool malformed_exponent = false;

        while (isdigit((unsigned char)lexer_current(lexer)) ||
               (lexer_current(lexer) == '.' && !has_dot &&
                isdigit((unsigned char)lexer_peek_char(lexer, 1)))) {
            if (lexer_current(lexer) == '.') has_dot = true;
            char value = lexer_advance_char(lexer);
            if (idx + 1 < sizeof(buffer)) buffer[idx++] = value;
            else too_long = true;
        }
        if (lexer_current(lexer) == 'e' || lexer_current(lexer) == 'E') {
            char value = lexer_advance_char(lexer);
            if (idx + 1 < sizeof(buffer)) buffer[idx++] = value;
            else too_long = true;
            if (lexer_current(lexer) == '+' || lexer_current(lexer) == '-') {
                value = lexer_advance_char(lexer);
                if (idx + 1 < sizeof(buffer)) buffer[idx++] = value;
                else too_long = true;
            }
            if (!isdigit((unsigned char)lexer_current(lexer))) {
                malformed_exponent = true;
            } else {
                while (isdigit((unsigned char)lexer_current(lexer))) {
                    value = lexer_advance_char(lexer);
                    if (idx + 1 < sizeof(buffer)) buffer[idx++] = value;
                    else too_long = true;
                }
            }
        }
        buffer[idx] = '\0';
        if (too_long) {
            token = lexer_create_token(lexer, TOKEN_ERROR, "número demasiado largo");
            milena_error_set(&lexer->error, MILENA_ERR_PARSE,
                             token.line, token.column, 0,
                             "El número supera el límite de 255 caracteres");
            lexer->current_token = token;
            return token;
        }
        if (malformed_exponent) {
            token = lexer_create_token(lexer, TOKEN_ERROR, "exponente numérico no válido");
            milena_error_set(&lexer->error, MILENA_ERR_PARSE,
                             token.line, token.column, 0,
                             "El exponente de un número debe contener dígitos");
            lexer->current_token = token;
            return token;
        }
        errno = 0;
        char *end = NULL;
        double number = strtod(buffer, &end);
        if (end != buffer + idx || errno == ERANGE || !isfinite(number)) {
            token = lexer_create_token(lexer, TOKEN_ERROR, "número fuera de rango");
            milena_error_set(&lexer->error, MILENA_ERR_PARSE,
                             token.line, token.column, 0,
                             "El literal numérico no es finito o está fuera de rango");
            lexer->current_token = token;
            return token;
        }
        token = lexer_create_token(lexer, TOKEN_NUMERO, buffer);
        token.number_value = number;
        lexer->current_token = token;
        return token;
    }
    
    // Identificadores y palabras clave
    if (is_identifier_start(c)) {
        char buffer[MAX_TOKEN_LEN];
        size_t idx = 0;
        
        bool too_long = false;
        while (is_identifier_char(lexer_current(lexer))) {
            char value = lexer_advance_char(lexer);
            if (idx + 1 < sizeof(buffer)) buffer[idx++] = value;
            else too_long = true;
        }
        
        if (too_long) {
            token = lexer_create_token(lexer, TOKEN_ERROR, "identificador demasiado largo");
            milena_error_set(&lexer->error, MILENA_ERR_PARSE,
                             token.line, token.column, 0,
                             "El identificador supera el límite de 255 caracteres");
            lexer->current_token = token;
            return token;
        }
        buffer[idx] = '\0';
        MilenaTokenType type = keyword_type(buffer);
        token = lexer_create_token(lexer, type, buffer);
        
        if (type == TOKEN_BOOLEANO) {
            token.number_value = strcmp(buffer, "verdadero") == 0 ? 1.0 : 0.0;
        }
        
        lexer->current_token = token;
        return token;
    }
    
    // Carácter desconocido
    lexer_advance_char(lexer);
    token = lexer_create_token(lexer, TOKEN_ERROR, "carácter desconocido");
    char err_msg[64];
    snprintf(err_msg, sizeof(err_msg), "Carácter inesperado: '%c'", c);
    milena_error_set(&lexer->error, MILENA_ERR_PARSE, token.line, token.column, 0, err_msg);
    lexer->current_token = token;
    return token;
}

Token lexer_peek_token(Lexer *lexer) {
    size_t old_pos = lexer->position;
    size_t old_line = lexer->line;
    size_t old_col = lexer->column;
    size_t old_token_start_offset = lexer->token_start_offset;
    size_t old_token_start_line = lexer->token_start_line;
    size_t old_token_start_column = lexer->token_start_column;
    Token prev = lexer->previous_token;
    Token curr = lexer->current_token;
    MilenaError old_error = lexer->error;
    
    Token token = lexer_next_token(lexer);
    
    lexer->position = old_pos;
    lexer->line = old_line;
    lexer->column = old_col;
    lexer->token_start_offset = old_token_start_offset;
    lexer->token_start_line = old_token_start_line;
    lexer->token_start_column = old_token_start_column;
    lexer->previous_token = prev;
    lexer->current_token = curr;
    lexer->error = old_error;
    
    return token;
}

void lexer_advance_token(Lexer *lexer) {
    if (!lexer) return;
    lexer->current_token = lexer_next_token(lexer);
}

bool lexer_match(Lexer *lexer, MilenaTokenType type) {
    return lexer->current_token.type == type;
}

bool lexer_expect(Lexer *lexer, MilenaTokenType type, const char *error_msg) {
    if (lexer->current_token.type != type) {
        milena_error_set(&lexer->error, MILENA_ERR_PARSE, lexer->current_token.line, lexer->current_token.column, 0, error_msg);
        return false;
    }
    return true;
}

const char *token_type_name(MilenaTokenType type) {
    static const char *const names[TOKEN_TYPE_COUNT] = {
        "EOF", "ERROR", "ANALISIS", "DATOS", "ESTADISTICA", "DATASET",
        "LIMPIAR", "TRANSFORMAR", "VISUALIZAR", "EXPORTAR", "FILTRAR",
        "AGRUPAR", "RESUMIR", "CARGAR", "NULOS", "DUPLICADOS",
        "CONDICION", "EXTRAER", "TOTAL", "PERIODO", "FUNCION_FORMA",
        "FUNCION_DIMENSIONES", "FUNCION_TAMANO", "FUNCION_SUMA",
        "FUNCION_MEDIA", "FUNCION_MINIMO", "FUNCION_MAXIMO",
        "FUNCION_VARIANZA", "FUNCION_DESVIACION", "FUNCION_MEDIANA",
        "FUNCION_PERCENTIL", "CONCEPTO_EJE", "CONCEPTO_CONSERVAR",
        "CONCEPTO_DIMENSIONES", "CONCEPTO_SIN", "VARIABLE", "FUNCION",
        "RETORNAR", "SI", "SINO", "DESDE", "PROCESAR", "POR", "LOTES",
        "FILAS", "GUARDAR", "RESULTADO", "EN", "DE", "CON", "REGISTROS",
        "HASTA", "MIB", "CONTAR", "PUNTO", "NUMERAL", "LLAVE_IZQ",
        "LLAVE_DER", "PAR_IZQ", "PAR_DER", "CORCHETE_IZQ",
        "CORCHETE_DER", "DOS_PUNTOS", "COMA", "PUNTO_Y_COMA", "IGUAL",
        "IGUAL_IGUAL", "DISTINTO", "MAYOR", "MAYOR_IGUAL", "MENOR",
        "MENOR_IGUAL", "MAS", "MENOS", "POR", "DIV", "ASIGNACION",
        "IDENTIFICADOR", "CADENA", "NUMERO", "BOOLEANO", "COMMENT"
    };
    if ((unsigned)type >= (unsigned)TOKEN_TYPE_COUNT) return "DESCONOCIDO";
    return names[type];
}

bool token_is_keyword(MilenaTokenType type) {
    return type >= TOKEN_KW_ANALISIS && type <= TOKEN_KW_CONTAR;
}

bool token_is_operator(MilenaTokenType type) {
    return (type >= TOKEN_IGUAL && type <= TOKEN_DIV) || 
           type == TOKEN_ASIGNACION;
}
