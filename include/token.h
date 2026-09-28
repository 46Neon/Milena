#ifndef MILENA_TOKEN_H
#define MILENA_TOKEN_H

#include "common.h"

#ifndef MAX_TOKEN_LEN
#define MAX_TOKEN_LEN 256
#endif

typedef enum {
    TOKEN_EOF = 0,
    TOKEN_ERROR,

    // Palabras clave en español
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
    TOKEN_FUNCION_FORMA,
    TOKEN_FUNCION_DIMENSIONES,
    TOKEN_FUNCION_TAMANO,
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
    TOKEN_KW_VARIABLE,
    TOKEN_KW_FUNCION,
    TOKEN_KW_RETORNAR,
    TOKEN_KW_SI,
    TOKEN_KW_SINO,
    TOKEN_KW_DESDE,
    TOKEN_KW_PROCESAR,
    TOKEN_KW_POR,
    TOKEN_KW_LOTES,
    TOKEN_KW_FILAS,
    TOKEN_KW_GUARDAR,
    TOKEN_KW_RESULTADO,
    TOKEN_KW_EN,
    TOKEN_KW_DE,
    TOKEN_KW_CON,
    TOKEN_KW_REGISTROS,
    TOKEN_KW_HASTA,
    TOKEN_KW_MIB,
    TOKEN_KW_CONTAR,

    // Separadores
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

    // Operadores
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

    // Literales
    TOKEN_IDENTIFICADOR,
    TOKEN_CADENA,
    TOKEN_NUMERO,
    TOKEN_BOOLEANO,

    // Especiales
    TOKEN_COMMENT,
    TOKEN_TYPE_COUNT
} MilenaTokenType;

/* The Windows SDK also declares TokenType; retain the legacy alias elsewhere. */
#ifndef _WIN32
typedef MilenaTokenType TokenType;
#endif

typedef struct Token {
    MilenaTokenType type;
    char lexeme[MAX_TOKEN_LEN];
    /* One-based start and exclusive end coordinates; offsets are source bytes. */
    size_t line;
    size_t column;
    size_t end_line;
    size_t end_column;
    size_t start_offset;
    size_t end_offset;
    double number_value;
} Token;

const char *token_type_name(MilenaTokenType type);
bool token_is_keyword(MilenaTokenType type);
bool token_is_operator(MilenaTokenType type);

#endif
