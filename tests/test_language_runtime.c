#include "language_runtime.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FALLO: %s\n", (message)); \
            return 1; \
        } \
    } while (0)

static bool read_stream(FILE *stream, char *buffer, size_t size) {
    if (!stream || !buffer || size == 0) return false;
    if (fflush(stream) != 0 || fseek(stream, 0, SEEK_SET) != 0) return false;
    size_t count = fread(buffer, 1, size - 1, stream);
    buffer[count] = '\0';
    return true;
}

int main(void) {
    const char *source =
        ".analisis prueba {\n"
        "  arreglo valores = [1, 2, 3, 4];\n"
        "  suma(valores);\n"
        "  media(valores, eje 0);\n"
        "  mediana(valores);\n"
        "  percentil(valores, 75, eje 0, conservar dimensiones);\n"
        "}\n";

    FILE *output = tmpfile();
    CHECK(output != NULL, "No se pudo crear la salida temporal");
    MilenaError error;
    milena_error_clear(&error);
    CHECK(milena_run_array_program(source, output, &error) == MILENA_OK,
          error.message);

    char text[4096];
    CHECK(read_stream(output, text, sizeof(text)),
          "No se pudo leer la salida del runtime");
    fclose(output);
    CHECK(strstr(text, "SUMA(valores) = 10") != NULL, "Suma incorrecta");
    CHECK(strstr(text, "MEDIA(valores) = 2.5") != NULL, "Media incorrecta");
    CHECK(strstr(text, "MEDIANA(valores) = 2.5") != NULL, "Mediana incorrecta");
    CHECK(strstr(text, "PERCENTIL(valores) = 3.25") != NULL,
          "Percentil incorrecto");
    CHECK(strstr(text, "shape=(1)") != NULL, "keepdims no se conservó");

    const char *invalid =
        ".analisis error { media(variable_no_declarada); }";
    milena_error_clear(&error);
    CHECK(milena_run_array_program(invalid, NULL, &error) == MILENA_ERR_PARSE,
          "El programa inválido no produjo error de parseo");
    CHECK(strstr(error.message, "no ha sido declarado") != NULL,
          "El primer diagnóstico fue sobrescrito");

    puts("language runtime: parser + AST + arrays OK");
    return 0;
}
