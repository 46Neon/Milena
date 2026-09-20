#include "lexer.h"
#include <ctype.h>

static char lexer_current(const Lexer *lexer) {
    return lexer->position < lexer->length ? lexer->source[lexer->position] : '\0';
}

static char lexer_peek_char(const Lexer *lexer, size_t offset) {
    if (offset > lexer->length - lexer->position) return '\0';
    size_t position = lexer->position + offset;
    return position < lexer->length ? lexer->source[position] : '\0';
}

static char lexer_advance_char(Lexer *lexer) {
    char c = lexer_current(lexer);
    if (c == '\0') return c;
    lexer->position++;
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    return c;
}

static Token lexer_token(TokenType type, const char *lexeme, int line, int column) {
    Token token = {0};
    token.type = type;
    token.line = line;
    token.column = column;
    if (lexeme) {
        strncpy(token.lexeme, lexeme, sizeof(token.lexeme) - 1);
        token.lexeme[sizeof(token.lexeme) - 1] = '\0';
    }
    return token;
}

static Token lexer_error(Lexer *lexer, const char *message, int line, int column) {
    milena_error_set(&lexer->error, MILENA_ERR_PARSE, (size_t)line,
                     (size_t)column, 0, message);
    return lexer_token(TOKEN_ERROR, message, line, column);
}

static bool identifier_start(char c) {
    unsigned char byte = (unsigned char)c;
    return isalpha(byte) != 0 || c == '_' || byte >= 0x80;
}

static bool identifier_part(char c) {
    unsigned char byte = (unsigned char)c;
    return isalnum(byte) != 0 || c == '_' || byte >= 0x80;
}

static TokenType keyword_type(const char *text) {
    static const struct {
        const char *text;
        TokenType type;
    } keywords[] = {
        {"analisis", TOKEN_KW_ANALISIS}, {"datos", TOKEN_KW_DATOS},
        {"estadistica", TOKEN_KW_ESTADISTICA}, {"dataset", TOKEN_KW_DATASET},
        {"limpiar", TOKEN_KW_LIMPIAR}, {"transformar", TOKEN_KW_TRANSFORMAR},
        {"visualizar", TOKEN_KW_VISUALIZAR}, {"exportar", TOKEN_KW_EXPORTAR},
        {"filtrar", TOKEN_KW_FILTRAR}, {"agrupar", TOKEN_KW_AGRUPAR},
        {"resumir", TOKEN_KW_RESUMIR}, {"cargar", TOKEN_KW_CARGAR},
        {"nulos", TOKEN_KW_NULOS}, {"duplicados", TOKEN_KW_DUPLICADOS},
        {"condicion", TOKEN_KW_CONDICION}, {"extraer", TOKEN_KW_EXTRAER},
        {"total", TOKEN_KW_TOTAL}, {"periodo", TOKEN_KW_PERIODO},
        {"suma", TOKEN_FUNCION_SUMA}, {"media", TOKEN_FUNCION_MEDIA},
        {"minimo", TOKEN_FUNCION_MINIMO}, {"maximo", TOKEN_FUNCION_MAXIMO},
        {"varianza", TOKEN_FUNCION_VARIANZA},
        {"desviacion_estandar", TOKEN_FUNCION_DESVIACION},
        {"mediana", TOKEN_FUNCION_MEDIANA}, {"percentil", TOKEN_FUNCION_PERCENTIL},
        {"eje", TOKEN_CONCEPTO_EJE}, {"conservar", TOKEN_CONCEPTO_CONSERVAR},
        {"dimensiones", TOKEN_CONCEPTO_DIMENSIONES}, {"sin", TOKEN_CONCEPTO_SIN}
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strcmp(text, keywords[i].text) == 0) return keywords[i].type;
    }
    if (strcmp(text, "verdadero") == 0 || strcmp(text, "falso") == 0) {
        return TOKEN_BOOLEANO;
    }
    return TOKEN_IDENTIFICADOR;
}

static void skip_space_and_comments(Lexer *lexer) {
    for (;;) {
        while (isspace((unsigned char)lexer_current(lexer)) != 0) {
            lexer_advance_char(lexer);
        }
        if (lexer_current(lexer) != '/' || lexer_peek_char(lexer, 1) != '/') return;
        while (lexer_current(lexer) != '\0' && lexer_current(lexer) != '\n') {
            lexer_advance_char(lexer);
        }
    }
}

void lexer_init(Lexer *lexer, const char *source) {
    if (!lexer) return;
    memset(lexer, 0, sizeof(*lexer));
    lexer->source = source ? source : "";
    lexer->length = strlen(lexer->source);
    lexer->line = 1;
    lexer->column = 1;
    milena_error_clear(&lexer->error);
    lexer->current_token = lexer_token(TOKEN_EOF, "EOF", 1, 1);
    lexer->previous_token = lexer->current_token;
}

Token lexer_next_token(Lexer *lexer) {
    if (!lexer) return lexer_token(TOKEN_ERROR, "lexer nulo", 0, 0);
    lexer->previous_token = lexer->current_token;
    skip_space_and_comments(lexer);

    int line = lexer->line;
    int column = lexer->column;
    char c = lexer_current(lexer);
    Token token;

    if (c == '\0') {
        token = lexer_token(TOKEN_EOF, "EOF", line, column);
    } else if (identifier_start(c)) {
        char buffer[MAX_TOKEN_LEN];
        size_t length = 0;
        bool too_long = false;
        while (identifier_part(lexer_current(lexer))) {
            char part = lexer_advance_char(lexer);
            if (length + 1 < sizeof(buffer)) buffer[length++] = part;
            else too_long = true;
        }
        buffer[length] = '\0';
        if (too_long) token = lexer_error(lexer, "Identificador demasiado largo", line, column);
        else {
            TokenType type = keyword_type(buffer);
            token = lexer_token(type, buffer, line, column);
            if (type == TOKEN_BOOLEANO) {
                token.number_value = strcmp(buffer, "verdadero") == 0 ? 1.0 : 0.0;
            }
        }
    } else if (isdigit((unsigned char)c) != 0 ||
               (c == '-' && isdigit((unsigned char)lexer_peek_char(lexer, 1)) != 0)) {
        char buffer[MAX_TOKEN_LEN];
        size_t length = 0;
        bool too_long = false;
#define APPEND_NUMBER_CHAR() do { \
            char part = lexer_advance_char(lexer); \
            if (length + 1 < sizeof(buffer)) buffer[length++] = part; \
            else too_long = true; \
        } while (0)
        if (lexer_current(lexer) == '-') APPEND_NUMBER_CHAR();
        while (isdigit((unsigned char)lexer_current(lexer)) != 0) APPEND_NUMBER_CHAR();
        if (lexer_current(lexer) == '.' &&
            isdigit((unsigned char)lexer_peek_char(lexer, 1)) != 0) {
            APPEND_NUMBER_CHAR();
            while (isdigit((unsigned char)lexer_current(lexer)) != 0) APPEND_NUMBER_CHAR();
        }
        if (lexer_current(lexer) == 'e' || lexer_current(lexer) == 'E') {
            char next = lexer_peek_char(lexer, 1);
            char after_sign = lexer_peek_char(lexer, 2);
            if (isdigit((unsigned char)next) != 0 ||
                ((next == '+' || next == '-') && isdigit((unsigned char)after_sign) != 0)) {
                APPEND_NUMBER_CHAR();
                if (lexer_current(lexer) == '+' || lexer_current(lexer) == '-') APPEND_NUMBER_CHAR();
                while (isdigit((unsigned char)lexer_current(lexer)) != 0) APPEND_NUMBER_CHAR();
            }
        }
#undef APPEND_NUMBER_CHAR
        buffer[length] = '\0';
        if (too_long) token = lexer_error(lexer, "Número demasiado largo", line, column);
        else {
            char *end = NULL;
            errno = 0;
            double value = strtod(buffer, &end);
            if (errno == ERANGE || !end || *end != '\0' || !isfinite(value)) {
                token = lexer_error(lexer, "Número inválido", line, column);
            } else {
                token = lexer_token(TOKEN_NUMERO, buffer, line, column);
                token.number_value = value;
            }
        }
    } else if (c == '"') {
        char buffer[MAX_TOKEN_LEN];
        size_t length = 0;
        bool too_long = false;
        bool closed = false;
        lexer_advance_char(lexer);
        while (lexer_current(lexer) != '\0') {
            if (lexer_current(lexer) == '"') {
                lexer_advance_char(lexer);
                closed = true;
                break;
            }
            char value = lexer_advance_char(lexer);
            if (value == '\\' && lexer_current(lexer) != '\0') {
                char escaped = lexer_advance_char(lexer);
                if (escaped == 'n') value = '\n';
                else if (escaped == 't') value = '\t';
                else if (escaped == 'r') value = '\r';
                else value = escaped;
            }
            if (length + 1 < sizeof(buffer)) buffer[length++] = value;
            else too_long = true;
        }
        buffer[length] = '\0';
        if (!closed) token = lexer_error(lexer, "Cadena sin cerrar", line, column);
        else if (too_long) token = lexer_error(lexer, "Cadena demasiado larga", line, column);
        else token = lexer_token(TOKEN_CADENA, buffer, line, column);
    } else {
        lexer_advance_char(lexer);
        switch (c) {
            case '.': token = lexer_token(TOKEN_PUNTO, ".", line, column); break;
            case '#': token = lexer_token(TOKEN_NUMERAL, "#", line, column); break;
            case '{': token = lexer_token(TOKEN_LLAVE_IZQ, "{", line, column); break;
            case '}': token = lexer_token(TOKEN_LLAVE_DER, "}", line, column); break;
            case '(': token = lexer_token(TOKEN_PAR_IZQ, "(", line, column); break;
            case ')': token = lexer_token(TOKEN_PAR_DER, ")", line, column); break;
            case '[': token = lexer_token(TOKEN_CORCHETE_IZQ, "[", line, column); break;
            case ']': token = lexer_token(TOKEN_CORCHETE_DER, "]", line, column); break;
            case ':': token = lexer_token(TOKEN_DOS_PUNTOS, ":", line, column); break;
            case ',': token = lexer_token(TOKEN_COMA, ",", line, column); break;
            case ';': token = lexer_token(TOKEN_PUNTO_Y_COMA, ";", line, column); break;
            case '+': token = lexer_token(TOKEN_MAS, "+", line, column); break;
            case '-': token = lexer_token(TOKEN_MENOS, "-", line, column); break;
            case '*': token = lexer_token(TOKEN_POR, "*", line, column); break;
            case '/': token = lexer_token(TOKEN_DIV, "/", line, column); break;
            case '=':
                if (lexer_current(lexer) == '=') {
                    lexer_advance_char(lexer);
                    token = lexer_token(TOKEN_IGUAL_IGUAL, "==", line, column);
                } else token = lexer_token(TOKEN_IGUAL, "=", line, column);
                break;
            case '!':
                if (lexer_current(lexer) == '=') {
                    lexer_advance_char(lexer);
                    token = lexer_token(TOKEN_DISTINTO, "!=", line, column);
                } else token = lexer_error(lexer, "Se esperaba '=' después de '!'", line, column);
                break;
            case '>':
                if (lexer_current(lexer) == '=') {
                    lexer_advance_char(lexer);
                    token = lexer_token(TOKEN_MAYOR_IGUAL, ">=", line, column);
                } else token = lexer_token(TOKEN_MAYOR, ">", line, column);
                break;
            case '<':
                if (lexer_current(lexer) == '=') {
                    lexer_advance_char(lexer);
                    token = lexer_token(TOKEN_MENOR_IGUAL, "<=", line, column);
                } else token = lexer_token(TOKEN_MENOR, "<", line, column);
                break;
            default: token = lexer_error(lexer, "Carácter inesperado", line, column); break;
        }
    }
    lexer->current_token = token;
    return token;
}

Token lexer_peek_token(Lexer *lexer) {
    if (!lexer) return lexer_token(TOKEN_ERROR, "lexer nulo", 0, 0);
    size_t position = lexer->position;
    int line = lexer->line;
    int column = lexer->column;
    Token previous = lexer->previous_token;
    Token current = lexer->current_token;
    MilenaError error = lexer->error;
    Token token = lexer_next_token(lexer);
    lexer->position = position;
    lexer->line = line;
    lexer->column = column;
    lexer->previous_token = previous;
    lexer->current_token = current;
    lexer->error = error;
    return token;
}

void lexer_advance_token(Lexer *lexer) {
    if (lexer) (void)lexer_next_token(lexer);
}

bool lexer_match(Lexer *lexer, TokenType type) {
    return lexer && lexer->current_token.type == type;
}

bool lexer_expect(Lexer *lexer, TokenType type, const char *error_msg) {
    if (!lexer || lexer->current_token.type != type) {
        if (lexer) {
            milena_error_set(&lexer->error, MILENA_ERR_PARSE,
                             (size_t)lexer->current_token.line,
                             (size_t)lexer->current_token.column, 0, error_msg);
        }
        return false;
    }
    return true;
}

const char *token_type_name(TokenType type) {
    static const char *const names[TOKEN_TYPE_COUNT] = {
        "EOF", "ERROR", "ANALISIS", "DATOS", "ESTADISTICA", "DATASET",
        "LIMPIAR", "TRANSFORMAR", "VISUALIZAR", "EXPORTAR", "FILTRAR",
        "AGRUPAR", "RESUMIR", "CARGAR", "NULOS", "DUPLICADOS", "CONDICION",
        "EXTRAER", "TOTAL", "PERIODO", "FUNCION_SUMA", "FUNCION_MEDIA",
        "FUNCION_MINIMO", "FUNCION_MAXIMO", "FUNCION_VARIANZA",
        "FUNCION_DESVIACION", "FUNCION_MEDIANA", "FUNCION_PERCENTIL",
        "CONCEPTO_EJE", "CONCEPTO_CONSERVAR", "CONCEPTO_DIMENSIONES",
        "CONCEPTO_SIN", "PUNTO", "NUMERAL", "LLAVE_IZQ", "LLAVE_DER",
        "PAR_IZQ", "PAR_DER", "CORCHETE_IZQ", "CORCHETE_DER", "DOS_PUNTOS",
        "COMA", "PUNTO_Y_COMA", "IGUAL", "IGUAL_IGUAL", "DISTINTO", "MAYOR",
        "MAYOR_IGUAL", "MENOR", "MENOR_IGUAL", "MAS", "MENOS", "POR", "DIV",
        "ASIGNACION", "IDENTIFICADOR", "CADENA", "NUMERO", "BOOLEANO",
        "COMMENT"
    };
    if ((unsigned)type >= (unsigned)TOKEN_TYPE_COUNT) return "DESCONOCIDO";
    return names[type];
}

bool token_is_keyword(TokenType type) {
    return type >= TOKEN_KW_ANALISIS && type <= TOKEN_CONCEPTO_SIN;
}

bool token_is_operator(TokenType type) {
    return (type >= TOKEN_IGUAL && type <= TOKEN_DIV) || type == TOKEN_ASIGNACION;
}
