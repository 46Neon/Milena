#ifndef MILENA_TOKEN_H
#define MILENA_TOKEN_H

#include "common.h"

#ifndef MAX_TOKEN_LEN
#define MAX_TOKEN_LEN 256
#endif

typedef enum {
    TOKEN_EOF = 0,
    TOKEN_ERROR,

    /* Palabras clave en español. */
    TOKEN_KW_ANALISIS,
    TOKEN_KW_DATOS,
    TOKEN_KW_ESTADISTICA,
    TOKEN_KW_DATASET,
    TOKEN_KW_LIMPIAR,
    TOKEN_KW_TRANSFORMAR,
    TOKEN_KW_VISUALIZAR,
    TOKEN_KW_EXPORTAR,
    TOKEN_KW_FILTRAR,
    TOKEN_KW_AGRUPAR,
    TOKEN_KW_RESUMIR,
    TOKEN_KW_CARGAR,
    TOKEN_KW_NULOS,
    TOKEN_KW_DUPLICADOS,
    TOKEN_KW_CONDICION,
    TOKEN_KW_EXTRAER,
    TOKEN_KW_TOTAL,
    TOKEN_KW_PERIODO,

    /* Operaciones y opciones estadísticas en español. */
    TOKEN_FUNCION_SUMA,
    TOKEN_FUNCION_MEDIA,
    TOKEN_FUNCION_MINIMO,
    TOKEN_FUNCION_MAXIMO,
    TOKEN_FUNCION_VARIANZA,
    TOKEN_FUNCION_DESVIACION,
    TOKEN_FUNCION_MEDIANA,
    TOKEN_FUNCION_PERCENTIL,
    TOKEN_CONCEPTO_EJE,
    TOKEN_CONCEPTO_CONSERVAR,
    TOKEN_CONCEPTO_DIMENSIONES,
    TOKEN_CONCEPTO_SIN,

    /* Separadores. */
    TOKEN_PUNTO,
    TOKEN_NUMERAL,
    TOKEN_LLAVE_IZQ,
    TOKEN_LLAVE_DER,
    TOKEN_PAR_IZQ,
    TOKEN_PAR_DER,
    TOKEN_CORCHETE_IZQ,
    TOKEN_CORCHETE_DER,
    TOKEN_DOS_PUNTOS,
    TOKEN_COMA,
    TOKEN_PUNTO_Y_COMA,

    /* Operadores. */
    TOKEN_IGUAL,
    TOKEN_IGUAL_IGUAL,
    TOKEN_DISTINTO,
    TOKEN_MAYOR,
    TOKEN_MAYOR_IGUAL,
    TOKEN_MENOR,
    TOKEN_MENOR_IGUAL,
    TOKEN_MAS,
    TOKEN_MENOS,
    TOKEN_POR,
    TOKEN_DIV,
    TOKEN_ASIGNACION,

    /* Literales. */
    TOKEN_IDENTIFICADOR,
    TOKEN_CADENA,
    TOKEN_NUMERO,
    TOKEN_BOOLEANO,

    TOKEN_COMMENT,
    TOKEN_TYPE_COUNT
} TokenType;

typedef struct Token {
    TokenType type;
    char lexeme[MAX_TOKEN_LEN];
    int line;
    int column;
    double number_value;
} Token;

const char *token_type_name(TokenType type);
bool token_is_keyword(TokenType type);
bool token_is_operator(TokenType type);

#endif
