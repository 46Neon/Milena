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

    const char *zeros =
        ".analisis matriz {\n"
        "  arreglo matriz = ceros(2, 3);\n"
        "  media(matriz, eje 0);\n"
        "}\n";
    output = tmpfile();
    CHECK(output != NULL, "No se pudo crear la salida temporal de ceros");
    milena_error_clear(&error);
    CHECK(milena_run_array_program(zeros, output, &error) == MILENA_OK,
          error.message);
    CHECK(read_stream(output, text, sizeof(text)),
          "No se pudo leer la salida de ceros");
    CHECK(strstr(text, "MEDIA(matriz) = [0, 0, 0]") != NULL,
          "ceros o reducción por eje incorrectos");
    CHECK(strstr(text, "shape=(3)") != NULL,
          "La forma de la reducción de ceros es incorrecta");
    fclose(output);

    const char *invalid =
        ".analisis error { media(variable_no_declarada); }";
    milena_error_clear(&error);
    CHECK(milena_run_array_program(invalid, NULL, &error) == MILENA_ERR_PARSE,
          "El programa inválido no produjo error de parseo");
    CHECK(strstr(error.message, "no ha sido declarado") != NULL,
          "El primer diagnóstico fue sobrescrito");

    const char *csv_path = "test-language-runtime.csv";
    const char *json_path = "test-language-runtime.json";
    FILE *csv = fopen(csv_path, "wb");
    CHECK(csv != NULL, "No se pudo crear el CSV de runtime");
    fputs("precio,cantidad,ciudad,compro\n10,2,Caracas,1\n5,3,Maracaibo,0\n", csv);
    CHECK(fclose(csv) == 0, "No se pudo cerrar el CSV de runtime");
    const char *dataset_source =
        ".analisis ventas {\n"
        "  dataset cargar datos(\"test-language-runtime.csv\")\n"
        "  variable precio numerica\n"
        "  variable cantidad numerica\n"
        "  entrada categorica \"ciudad\"\n"
        "  salida binaria \"compro\"\n"
        "  .limpiar dataset { #nulos(\"eliminar\") }\n"
        "  .transformar dataset { #total(\"precio * cantidad\") }\n"
        "  .exportar { (\"test-language-runtime.json\") }\n"
        "}\n";
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(dataset_source, "test-language-runtime.milena",
                                     NULL, &error) == MILENA_OK,
          error.message);
    FILE *json = fopen(json_path, "rb");
    CHECK(json != NULL, "El runtime no creó el JSON del dataset");
    char json_text[4096];
    size_t json_size = fread(json_text, 1, sizeof(json_text) - 1, json);
    json_text[json_size] = '\0';
    fclose(json);
    CHECK(strstr(json_text, "total") != NULL,
          "La transformación no llegó al reporte unificado");
    CHECK(strstr(json_text, "numerica") != NULL,
          "El esquema del dataset no llegó al reporte unificado");
    CHECK(strstr(json_text, "entradas_categoricas") != NULL &&
          strstr(json_text, "ciudad") != NULL,
          "La entrada categórica no llegó al reporte unificado");
    CHECK(strstr(json_text, "salidas_binarias") != NULL &&
          strstr(json_text, "compro") != NULL,
          "La salida binaria no llegó al reporte unificado");
    remove(csv_path);
    remove(json_path);

    puts("language runtime: parser + AST + arrays + datasets OK");
    return 0;
}
