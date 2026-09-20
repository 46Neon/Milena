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
    const char *right_csv_path = "test-language-runtime-right.csv";
    FILE *csv = fopen(csv_path, "wb");
    CHECK(csv != NULL, "No se pudo crear el CSV de runtime");
    fputs("precio,cantidad,ciudad,compro,fecha\n10,2,Caracas,1,2026-01-10\n5,3,Maracaibo,0,2026-02-11\n9,4,,1,2026-03-12\n5,3,Maracaibo,0,2026-02-11\n-1,2,Maracaibo,0,2026-04-01\n", csv);
    CHECK(fclose(csv) == 0, "No se pudo cerrar el CSV de runtime");
    FILE *right_csv = fopen(right_csv_path, "wb");
    CHECK(right_csv != NULL, "No se pudo crear el CSV derecho");
    fputs("ciudad,region\nCaracas,Centro\nMaracaibo,Occidente\n", right_csv);
    CHECK(fclose(right_csv) == 0, "No se pudo cerrar el CSV derecho");
    const char *dataset_source =
        ".analisis ventas {\n"
        "  dataset cargar datos(\"test-language-runtime.csv\")\n"
        "  variable precio numerica\n"
        "  variable cantidad numerica\n"
        "  variable fecha texto\n"
        "  entrada categorica \"ciudad\"\n"
        "  salida binaria \"compro\"\n"
        "  .limpiar dataset { #nulos(\"eliminar\") #duplicados(\"eliminar\") }\n"
        "  .transformar dataset { #total(\"precio * cantidad\") #periodo(\"mes de fecha\") }\n"
        "  .ventas_validas { dataset, (filtrar) #condicion(\"total > 0\") }\n"
        "  .agrupar dataset { #por(\"ciudad\") #suma(\"total\") #media(\"total\") #conteo(\"total\") }\n"
        "  .unir { #derecha(\"test-language-runtime-right.csv\") #clave(\"ciudad\") }\n"
        "  .seleccionar { #columnas(\"ciudad,total_suma,region\") }\n"
        "  #perfil_avanzado(\"total_suma\")\n"
        "  #histograma(\"total_suma\")\n"
        "  #normalidad(\"total_suma\")\n"
        "  #tasa(\"total_suma,total_suma,200000\")\n"
        "  #poisson(\"total_suma,total_suma,200000\")\n"
        "  #correlacion(\"total_suma,total_suma\")\n"
        "  #wilcoxon(\"total_suma,total_suma\")\n"
        "  #chi_cuadrado(\"ciudad,region\")\n"
        "  #riesgo(\"ciudad,region,Caracas,Centro\")\n"
        "  #modelo_sst(\"ciudad,total_suma,region\")\n"
        "  #interes_simple(\"total_suma,total_suma,2\")\n"
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
    CHECK(strstr(json_text, "periodo") != NULL &&
          strstr(json_text, "2026-02") != NULL,
          "La extracción de periodo no llegó al reporte unificado");
    CHECK(strstr(json_text, "\"filas\": 2") != NULL,
          "La limpieza de nulos no se ejecutó sobre la tabla canónica");
    CHECK(strstr(json_text, "\"columnas\": 3") != NULL &&
          strstr(json_text, "region") != NULL &&
          strstr(json_text, "total_suma") != NULL &&
          strstr(json_text, "total_mean") != NULL &&
          strstr(json_text, "total_count") != NULL,
          "Las agregaciones no llegaron al reporte unificado");
    CHECK(strstr(json_text, "numerica") != NULL,
          "El esquema del dataset no llegó al reporte unificado");
    CHECK(strstr(json_text, "entradas_categoricas") != NULL &&
          strstr(json_text, "ciudad") != NULL,
          "La entrada categórica no llegó al reporte unificado");
    CHECK(strstr(json_text, "salidas_binarias") != NULL &&
          strstr(json_text, "compro") != NULL,
          "La salida binaria no llegó al reporte unificado");
    FILE *sst_json = fopen("test-language-runtime.json.sst.json", "rb");
    CHECK(sst_json != NULL, "El perfil SST canónico no creó su reporte");
    char sst_text[2048];
    size_t sst_size = fread(sst_text, 1, sizeof(sst_text) - 1, sst_json);
    sst_text[sst_size] = '\0';
    fclose(sst_json);
    CHECK(strstr(sst_text, "perfil_avanzado") != NULL &&
          strstr(sst_text, "total_suma") != NULL,
          "El perfil SST no usó la tabla canónica");
    FILE *histogram_json = fopen("test-language-runtime.json.histograma.json", "rb");
    CHECK(histogram_json != NULL, "El histograma SST canónico no creó su reporte");
    char histogram_text[2048];
    size_t histogram_size = fread(histogram_text, 1, sizeof(histogram_text) - 1, histogram_json);
    histogram_text[histogram_size] = '\0';
    fclose(histogram_json);
    CHECK(strstr(histogram_text, "histograma") != NULL &&
          strstr(histogram_text, "total_suma") != NULL,
          "El histograma SST no usó la tabla canónica");
    FILE *normality_json = fopen("test-language-runtime.json.normalidad.json", "rb");
    CHECK(normality_json != NULL, "La normalidad SST canónica no creó su reporte");
    char normality_text[2048];
    size_t normality_size = fread(normality_text, 1, sizeof(normality_text) - 1, normality_json);
    normality_text[normality_size] = '\0';
    fclose(normality_json);
    CHECK(strstr(normality_text, "normalidad") != NULL &&
          strstr(normality_text, "total_suma") != NULL,
          "La normalidad SST no usó la tabla canónica");
    FILE *rate_json = fopen("test-language-runtime.json.tasa.json", "rb");
    CHECK(rate_json != NULL, "La tasa SST canónica no creó su reporte");
    char rate_text[2048];
    size_t rate_size = fread(rate_text, 1, sizeof(rate_text) - 1, rate_json);
    rate_text[rate_size] = '\0';
    fclose(rate_json);
    CHECK(strstr(rate_text, "tasa") != NULL && strstr(rate_text, "incidentes") != NULL,
          "La tasa SST no usó la tabla canónica");
    FILE *poisson_json = fopen("test-language-runtime.json.poisson.json", "rb");
    CHECK(poisson_json != NULL, "Poisson SST canónico no creó su reporte");
    char poisson_text[2048];
    size_t poisson_size = fread(poisson_text, 1, sizeof(poisson_text) - 1, poisson_json);
    poisson_text[poisson_size] = '\0';
    fclose(poisson_json);
    CHECK(strstr(poisson_text, "poisson") != NULL &&
          strstr(poisson_text, "ic_inferior") != NULL,
          "Poisson SST no usó la tabla canónica");
    FILE *correlation_json = fopen("test-language-runtime.json.correlacion.json", "rb");
    CHECK(correlation_json != NULL, "La correlación SST canónica no creó su reporte");
    char correlation_text[2048];
    size_t correlation_size = fread(correlation_text, 1, sizeof(correlation_text) - 1, correlation_json);
    correlation_text[correlation_size] = '\0';
    fclose(correlation_json);
    CHECK(strstr(correlation_text, "correlacion") != NULL &&
          strstr(correlation_text, "coeficiente") != NULL,
          "La correlación SST no usó la tabla canónica");
    FILE *wilcoxon_json = fopen("test-language-runtime.json.wilcoxon.json", "rb");
    CHECK(wilcoxon_json != NULL, "Wilcoxon SST canónico no creó su reporte");
    char wilcoxon_text[2048];
    size_t wilcoxon_size = fread(wilcoxon_text, 1, sizeof(wilcoxon_text) - 1, wilcoxon_json);
    wilcoxon_text[wilcoxon_size] = '\0';
    fclose(wilcoxon_json);
    CHECK(strstr(wilcoxon_text, "wilcoxon") != NULL &&
          strstr(wilcoxon_text, "estadistico") != NULL,
          "Wilcoxon SST no usó la tabla canónica");
    FILE *chi_json = fopen("test-language-runtime.json.chi_cuadrado.json", "rb");
    CHECK(chi_json != NULL, "Chi cuadrado SST canónico no creó su reporte");
    char chi_text[2048];
    size_t chi_size = fread(chi_text, 1, sizeof(chi_text) - 1, chi_json);
    chi_text[chi_size] = '\0';
    fclose(chi_json);
    CHECK(strstr(chi_text, "chi_cuadrado") != NULL &&
          strstr(chi_text, "estadistico") != NULL,
          "Chi cuadrado SST no usó la tabla canónica");
    FILE *risk_json = fopen("test-language-runtime.json.riesgo.json", "rb");
    CHECK(risk_json != NULL, "Riesgo SST canónico no creó su reporte");
    char risk_text[2048];
    size_t risk_size = fread(risk_text, 1, sizeof(risk_text) - 1, risk_json);
    risk_text[risk_size] = '\0';
    fclose(risk_json);
    CHECK(strstr(risk_text, "riesgo_relativo") != NULL &&
          strstr(risk_text, "odds_ratio") != NULL,
          "Riesgo SST no usó la tabla canónica");
    FILE *model_json = fopen("test-language-runtime.json.modelo_sst.json", "rb");
    CHECK(model_json != NULL, "Modelo SST canónico no creó su reporte");
    char model_text[2048];
    size_t model_size = fread(model_text, 1, sizeof(model_text) - 1, model_json);
    model_text[model_size] = '\0';
    fclose(model_json);
    CHECK(strstr(model_text, "modelo_sst") != NULL &&
          strstr(model_text, "MilenaTable") != NULL,
          "Modelo SST no usó la tabla canónica");
    FILE *finance_json = fopen("test-language-runtime.json.interes_simple.json", "rb");
    CHECK(finance_json != NULL, "Finanzas canónicas no creó su reporte");
    char finance_text[2048];
    size_t finance_size = fread(finance_text, 1, sizeof(finance_text) - 1, finance_json);
    finance_text[finance_size] = '\0';
    fclose(finance_json);
    CHECK(strstr(finance_text, "interes_simple") != NULL &&
          strstr(finance_text, "finance") != NULL,
          "Finanzas no usó la tabla canónica");
    const char *summary_json_path = "test-language-runtime-summary.json";
    const char *summary_source =
        ".analisis resumen {\n"
        "  dataset cargar datos(\"test-language-runtime.csv\")\n"
        "  variable precio numerica\n"
        "  variable cantidad numerica\n"
        "  .transformar dataset { #total(\"precio * cantidad\") }\n"
        "  .resumir dataset { #suma(\"total\") #media(\"total\") #conteo(\"total\") #varianza(\"total\") #desviacion_estandar(\"total\") #mediana(\"total\") #percentil(\"total,50\") }\n"
        "  .exportar { (\"test-language-runtime-summary.json\") }\n"
        "}\n";
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(summary_source, "test-language-runtime-summary.milena",
                                     NULL, &error) == MILENA_OK,
          error.message);
    FILE *summary_json = fopen(summary_json_path, "rb");
    CHECK(summary_json != NULL, "El resumen no creó su reporte JSON");
    char summary_text[4096];
    size_t summary_size = fread(summary_text, 1, sizeof(summary_text) - 1, summary_json);
    summary_text[summary_size] = '\0';
    fclose(summary_json);
    CHECK(strstr(summary_text, "\"filas\": 1") != NULL &&
          strstr(summary_text, "total_mean") != NULL &&
          strstr(summary_text, "total_varianza") != NULL &&
          strstr(summary_text, "total_mediana") != NULL &&
          strstr(summary_text, "total_percentil") != NULL,
          "El bloque resumir no usó la tabla canónica");
    remove(summary_json_path);
    remove(csv_path);
    remove(right_csv_path);
    remove(json_path);
    remove("test-language-runtime.json.sst.json");
    remove("test-language-runtime.json.histograma.json");
    remove("test-language-runtime.json.normalidad.json");
    remove("test-language-runtime.json.tasa.json");
    remove("test-language-runtime.json.poisson.json");
    remove("test-language-runtime.json.correlacion.json");
    remove("test-language-runtime.json.wilcoxon.json");
    remove("test-language-runtime.json.chi_cuadrado.json");
    remove("test-language-runtime.json.riesgo.json");
    remove("test-language-runtime.json.modelo_sst.json");
    remove("test-language-runtime.json.interes_simple.json");

    puts("language runtime: parser + AST + arrays + datasets OK");
    return 0;
}
