#include "language_runtime.h"
#include "language_grouped_spill.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FALLO: %s\n", (message)); \
            return 1; \
        } \
    } while (0)

static bool read_file(const char *path, char *buffer, size_t size) {
    if (!path || !buffer || size < 2) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t count = fread(buffer, 1, size - 1, file);
    buffer[count] = '\0';
    bool ok = !ferror(file);
    fclose(file);
    return ok;
}

static bool write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    size_t length = strlen(content);
    bool ok = fwrite(content, 1, length, file) == length && fclose(file) == 0;
    if (!ok) fclose(file);
    return ok;
}

static int run_arrays(void) {
    MilenaError error;
    const char *source =
        ".analisis prueba {\n"
        "  arreglo valores = [1, 2, 3, 4];\n"
        "  suma(valores);\n"
        "  media(valores, eje 0);\n"
        "  mediana(valores);\n"
        "  percentil(valores, 75, eje 0, conservar dimensiones);\n"
        "}\n";
    FILE *output = tmpfile();
    CHECK(output != NULL, "arrays: no se pudo crear la salida temporal");
    milena_error_clear(&error);
    CHECK(milena_run_array_program(source, output, &error) == MILENA_OK,
          error.message);
    char text[4096];
    CHECK(fseek(output, 0, SEEK_SET) == 0, "arrays: no se pudo rebobinar la salida");
    size_t count = fread(text, 1, sizeof(text) - 1, output);
    text[count] = '\0';
    fclose(output);
    CHECK(strstr(text, "SUMA(valores) = 10") != NULL, "arrays: suma incorrecta");
    CHECK(strstr(text, "MEDIA(valores) = 2.5") != NULL, "arrays: media incorrecta");
    CHECK(strstr(text, "MEDIANA(valores) = 2.5") != NULL, "arrays: mediana incorrecta");
    CHECK(strstr(text, "PERCENTIL(valores) = 3.25") != NULL,
          "arrays: percentil incorrecto");
    CHECK(strstr(text, "shape=(1)") != NULL, "arrays: keepdims no se conservó");

    const char *zeros =
        ".analisis matriz {\n"
        "  arreglo matriz = ceros(2, 3);\n"
        "  media(matriz, eje 0);\n"
        "}\n";
    output = tmpfile();
    CHECK(output != NULL, "arrays: no se pudo crear la salida de ceros");
    milena_error_clear(&error);
    CHECK(milena_run_array_program(zeros, output, &error) == MILENA_OK,
          error.message);
    CHECK(fseek(output, 0, SEEK_SET) == 0, "arrays: no se pudo leer ceros");
    count = fread(text, 1, sizeof(text) - 1, output);
    text[count] = '\0';
    fclose(output);
    CHECK(strstr(text, "MEDIA(matriz) = [0, 0, 0]") != NULL,
          "arrays: ceros o reducción por eje incorrectos");
    CHECK(strstr(text, "shape=(3)") != NULL,
          "arrays: forma de reducción incorrecta");

    const char *invalid = ".analisis error { media(variable_no_declarada); }";
    milena_error_clear(&error);
    CHECK(milena_run_array_program(invalid, NULL, &error) == MILENA_ERR_PARSE,
          "arrays: programa inválido sin error");
    CHECK(strstr(error.message, "no ha sido declarado") != NULL,
          "arrays: diagnóstico de declaración perdido");
    return 0;
}

static int run_dataset_pipeline(void) {
    const char *csv = "test-language-runtime-data.csv";
    const char *right = "test-language-runtime-right.csv";
    const char *output = "test-language-runtime-data.json";
    const char *csv_content =
        "precio,cantidad,ciudad,compro,fecha\n"
        "10,2,Caracas,1,2026-01-10\n"
        "5,3,Maracaibo,0,2026-02-11\n"
        "9,4,,1,2026-03-12\n"
        "5,3,Maracaibo,0,2026-02-11\n"
        "-1,2,Maracaibo,0,2026-04-01\n";
    const char *right_content =
        "ciudad,region\n"
        "Caracas,Centro\n"
        "Maracaibo,Occidente\n";
    CHECK(write_file(csv, csv_content), "dataset: no se pudo crear el CSV izquierdo");
    CHECK(write_file(right, right_content), "dataset: no se pudo crear el CSV derecho");

    const char *source =
        ".analisis ventas {\n"
        "  dataset cargar datos(\"test-language-runtime-data.csv\")\n"
        "  variable precio numerica\n"
        "  variable cantidad numerica\n"
        "  variable fecha texto\n"
        "  entrada categorica \"ciudad\"\n"
        "  salida binaria \"compro\"\n"
        "  .limpiar dataset { #nulos(\"eliminar\") #duplicados(\"eliminar\") }\n"
        "  .transformar dataset { #total(\"precio * cantidad\") #periodo(\"mes de fecha\") }\n"
        "  .agrupar dataset { #por(\"ciudad\") #suma(\"total\") #media(\"total\") #conteo(\"total\") }\n"
        "  .unir { #derecha(\"test-language-runtime-right.csv\") #clave(\"ciudad\") }\n"
        "  .seleccionar { #columnas(\"ciudad,total_suma,total_media,total_conteo,region\") }\n"
        "  #perfil_avanzado(\"total_suma\")\n"
        "  #histograma(\"total_suma\")\n"
        "  #tasa(\"total_suma,total_suma,200000\")\n"
        "  #poisson(\"total_suma,total_suma,200000\")\n"
        "  #chi_cuadrado(\"ciudad,region\")\n"
        "  #riesgo(\"ciudad,region,Caracas,Centro\")\n"
        "  #modelo_sst(\"ciudad,total_suma,region\")\n"
        "  #interes_simple(\"total_suma,total_suma,2\")\n"
        "  .exportar { (\"test-language-runtime-data.json\") }\n"
        "}\n";
    MilenaError error;
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(source, "test-language-runtime-data.milena",
                                     NULL, &error) == MILENA_OK,
          error.message);
    char text[8192];
    CHECK(read_file(output, text, sizeof(text)), "dataset: no se creó el JSON");
    CHECK(strstr(text, "\"filas\": 2") != NULL,
          "dataset: limpieza no llegó a la tabla");
    CHECK(strstr(text, "\"columnas\": 5") != NULL &&
          strstr(text, "region") != NULL &&
          strstr(text, "total_suma") != NULL &&
          strstr(text, "total_media") != NULL &&
          strstr(text, "total_conteo") != NULL,
          "dataset: agregaciones o unión incompletas");
    CHECK(strstr(text, "entradas_categoricas") != NULL &&
          strstr(text, "salidas_binarias") != NULL,
          "dataset: roles del esquema ausentes");
    CHECK(read_file("test-language-runtime-data.json.sst.json", text, sizeof(text)),
          "dataset: no se creó el perfil SST");
    CHECK(strstr(text, "perfil_avanzado") != NULL, "dataset: perfil SST incompleto");
    CHECK(read_file("test-language-runtime-data.json.histograma.json", text, sizeof(text)),
          "dataset: no se creó el histograma SST");
    CHECK(read_file("test-language-runtime-data.json.tasa.json", text, sizeof(text)),
          "dataset: no se creó la tasa SST");
    CHECK(read_file("test-language-runtime-data.json.poisson.json", text, sizeof(text)),
          "dataset: no se creó Poisson SST");
    CHECK(read_file("test-language-runtime-data.json.chi_cuadrado.json", text, sizeof(text)),
          "dataset: no se creó chi cuadrado SST");
    CHECK(read_file("test-language-runtime-data.json.riesgo.json", text, sizeof(text)),
          "dataset: no se creó riesgo SST");
    CHECK(read_file("test-language-runtime-data.json.modelo_sst.json", text, sizeof(text)),
          "dataset: no se creó modelo SST");
    CHECK(read_file("test-language-runtime-data.json.interes_simple.json", text, sizeof(text)),
          "dataset: no se creó interés simple");

    const char *limited_join =
        ".analisis join_limit {\n"
        "  dataset cargar datos(\"test-language-runtime-data.csv\")\n"
        "  variable ciudad texto\n"
        "  .unir { #derecha(\"test-language-runtime-right.csv\") #clave(\"ciudad\") #limites(67108864, 1) }\n"
        "  .exportar { (\"test-language-runtime-limited.json\") }\n"
        "}\n";
    remove("test-language-runtime-limited.json");
    CHECK(milena_run_dataset_program(limited_join,
        "test-language-runtime-limit.milena", NULL, &error) == MILENA_ERR_OVERFLOW,
        "join: debió rechazar la salida por superar el límite AST de filas");
    FILE *unexpected = fopen("test-language-runtime-limited.json", "rb");
    if (unexpected) fclose(unexpected);
    CHECK(unexpected == NULL,
        "join: publicó salida parcial después del límite");

    const char *large_right = "test-language-runtime-join-large.csv";
    char large_content[6000];
    const char *large_header = "ciudad,region\nCaracas,";
    size_t header_length = strlen(large_header);
    memcpy(large_content, large_header, header_length);
    memset(large_content + header_length, 'x', 5000);
    large_content[header_length + 5000] = '\n';
    large_content[header_length + 5001] = '\0';
    CHECK(write_file(large_right, large_content),
          "join: no se pudo crear el CSV de presupuesto");
    const char *memory_limited_join =
        ".analisis join_memory_limit {\n"
        "  dataset cargar datos(\"test-language-runtime-data.csv\")\n"
        "  variable ciudad texto\n"
        "  .unir { #derecha(\"test-language-runtime-join-large.csv\") #clave(\"ciudad\") #limites(4096, 100) }\n"
        "  .exportar { (\"test-language-runtime-memory-limited.json\") }\n"
        "}\n";
    remove("test-language-runtime-memory-limited.json");
    CHECK(milena_run_dataset_program(memory_limited_join,
        "test-language-runtime-memory-limit.milena", NULL, &error) ==
        MILENA_ERR_OVERFLOW,
        "join: debió rechazar la estimación de memoria sobre el presupuesto");
    unexpected = fopen("test-language-runtime-memory-limited.json", "rb");
    if (unexpected) fclose(unexpected);
    CHECK(unexpected == NULL,
        "join: publicó salida parcial después del límite de memoria");

    remove(csv); remove(right); remove(large_right); remove(output);
    remove("test-language-runtime-data.json.sst.json");
    remove("test-language-runtime-data.json.histograma.json");
    remove("test-language-runtime-data.json.tasa.json");
    remove("test-language-runtime-data.json.poisson.json");
    remove("test-language-runtime-data.json.chi_cuadrado.json");
    remove("test-language-runtime-data.json.riesgo.json");
    remove("test-language-runtime-data.json.modelo_sst.json");
    remove("test-language-runtime-data.json.interes_simple.json");
    remove("test-language-runtime-limited.json");
    remove("test-language-runtime-memory-limited.json");
    return 0;
}

static int run_typed_data_hir_runtime(void) {
    const char *csv = "test-hir-data-runtime.csv";
    const char *report = "test-hir-data-runtime.json";
    const char *content = "id,precio,cantidad\n1,2,3\n2,5,2\n3,4,1\n";
    CHECK(write_file(csv, content), "HIR runtime: no se pudo escribir el CSV de entrada");
    const char *source =
        ".analisis ventas {\n"
        " variable id numerica\n"
        " variable precio numerica\n"
        " variable cantidad numerica\n"
        " dataset cargar datos(\"test-hir-data-runtime.csv\")\n"
        " .transformar dataset { #total(\"precio * cantidad\") }\n"
        " .filtrar { #condicion(\"total >= 10\") }\n"
        " .seleccionar { #columnas(\"id,total\") }\n"
        " .exportar { (\"test-hir-data-runtime.json\") }\n"
        "}\n";
    MilenaError error;
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(source, "test-hir-data-runtime.milena",
                                     NULL, &error) == MILENA_OK,
          error.message);
    char text[4096];
    CHECK(read_file(report, text, sizeof(text)), "HIR runtime: no se publicó el reporte");
    CHECK(strstr(text, "\"filas\": 1") != NULL &&
          strstr(text, "total") != NULL && strstr(text, "precio") == NULL,
          "HIR runtime: la ruta canónica no ejecutó producto/filtro/proyección antes de exportar");

    const char *group_report = "test-hir-group-runtime.json";
    const char *group_source =
        ".analisis agrupado {\n"
        " variable id numerica\n"
        " variable precio numerica\n"
        " dataset cargar datos(\"test-hir-data-runtime.csv\")\n"
        " .agrupar dataset { #por(\"id\") #suma(\"precio\") }\n"
        " .exportar { (\"test-hir-group-runtime.json\") }\n"
        "}\n";
    CHECK(milena_run_dataset_program(group_source, "test-hir-group-runtime.milena",
                                     NULL, &error) == MILENA_OK,
          error.message);
    CHECK(read_file(group_report, text, sizeof(text)) &&
          strstr(text, "\"filas\": 3") != NULL &&
          strstr(text, "precio_suma") != NULL,
          "HIR runtime: agrupación canónica no llegó al reporte con su agregado");

    const char *clean_csv = "test-hir-clean-runtime.csv";
    const char *clean_report = "test-hir-clean-runtime.json";
    CHECK(write_file(clean_csv, "id,name\n1,A\n1,A\n2,B\n"),
          "HIR cleaning runtime: no se pudo escribir el CSV de prueba");
    const char *clean_source =
        ".analisis limpieza_canonica {\n"
        " dataset cargar datos(\"test-hir-clean-runtime.csv\")\n"
        " .limpiar dataset { #duplicados(\"eliminar\") }\n"
        " .exportar { (\"test-hir-clean-runtime.json\") }\n"
        "}\n";
    CHECK(milena_run_dataset_program(clean_source,
          "test-hir-clean-runtime.milena", NULL, &error) == MILENA_OK,
          error.message);
    CHECK(read_file(clean_report, text, sizeof(text)) &&
          strstr(text, "\"filas\": 2") != NULL,
          "HIR cleaning runtime: la deduplicación no llegó a la salida canónica");

    const char *right_csv = "test-hir-join-runtime-right.csv";
    const char *join_report = "test-hir-join-runtime.json";
    CHECK(write_file(right_csv, "id,region\n1,Norte\n2,Centro\n3,Sur\n"),
          "HIR join runtime: no se pudo escribir el CSV derecho");
    const char *join_source =
        ".analisis ventas_unidas {\n"
        " variable id numerica\n"
        " dataset cargar datos(\"test-hir-data-runtime.csv\")\n"
        " .unir { #derecha(\"test-hir-join-runtime-right.csv\") #clave(\"id\") }\n"
        " .exportar { (\"test-hir-join-runtime.json\") }\n"
        "}\n";
    CHECK(milena_run_dataset_program(join_source,
          "test-hir-join-runtime.milena", NULL, &error) == MILENA_OK,
          error.message);
    CHECK(read_file(join_report, text, sizeof(text)) &&
          strstr(text, "\"filas\": 3") != NULL &&
          strstr(text, "region") != NULL && strstr(text, "Norte") != NULL,
          "HIR join runtime: el pipeline canónico no enlazó ni reportó el dataset derecho");

    const char *missing_report = "test-hir-join-missing.json";
    const char *missing_join_source =
        ".analisis join_fuente_ausente {\n"
        " dataset cargar datos(\"test-hir-data-runtime.csv\")\n"
        " .unir { #derecha(\"test-hir-join-right-absent.csv\") #clave(\"id\") }\n"
        " .exportar { (\"test-hir-join-missing.json\") }\n"
        "}\n";
    remove(missing_report);
    remove("test-hir-join-right-absent.csv");
    CHECK(milena_run_dataset_program(missing_join_source,
          "test-hir-join-missing.milena", NULL, &error) != MILENA_OK,
          "HIR join runtime: debió fallar al abrir una fuente derecha inexistente");
    FILE *missing_output = fopen(missing_report, "rb");
    if (missing_output) fclose(missing_output);
    CHECK(missing_output == NULL,
          "HIR join runtime: publicó un reporte pese a faltar la fuente derecha");
    remove(csv);
    remove(report);
    remove(group_report);
    remove(clean_csv);
    remove(clean_report);
    remove(right_csv);
    remove(join_report);
    remove(missing_report);
    return 0;
}

static int run_inference_pipeline(void) {
    const char *csv = "test-language-runtime-inference-data.csv";
    const char *output = "test-language-runtime-inference-data.json";
    const char *content =
        "antes,despues\n"
        "1,2\n2,4\n3,6\n4,8\n5,10\n6,12\n7,14\n8,16\n"
        "9,18\n10,20\n11,22\n12,24\n";
    CHECK(write_file(csv, content), "inferencia: no se pudo crear el CSV");
    const char *source =
        ".analisis inferencia {\n"
        "  dataset cargar datos(\"test-language-runtime-inference-data.csv\")\n"
        "  variable antes numerica\n"
        "  variable despues numerica\n"
        "  #normalidad(\"antes\")\n"
        "  #correlacion(\"antes,despues\")\n"
        "  #wilcoxon(\"antes,despues\")\n"
        "  .exportar { (\"test-language-runtime-inference-data.json\") }\n"
        "}\n";
    MilenaError error;
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(source,
                                     "test-language-runtime-inference-data.milena",
                                     NULL, &error) == MILENA_OK,
          error.message);
    char text[4096];
    CHECK(read_file("test-language-runtime-inference-data.json.normalidad.json",
                    text, sizeof(text)) && strstr(text, "normalidad") != NULL,
          "inferencia: normalidad no llegó a la tabla");
    CHECK(read_file("test-language-runtime-inference-data.json.correlacion.json",
                    text, sizeof(text)) && strstr(text, "correlacion") != NULL,
          "inferencia: correlación no llegó a la tabla");
    CHECK(read_file("test-language-runtime-inference-data.json.wilcoxon.json",
                    text, sizeof(text)) && strstr(text, "wilcoxon") != NULL,
          "inferencia: Wilcoxon no llegó a la tabla");
    remove(csv); remove(output);
    remove("test-language-runtime-inference-data.json.normalidad.json");
    remove("test-language-runtime-inference-data.json.correlacion.json");
    remove("test-language-runtime-inference-data.json.wilcoxon.json");
    return 0;
}

static int run_summary_pipeline(void) {
    const char *csv = "test-language-runtime-summary-data.csv";
    const char *output = "test-language-runtime-summary-data.json";
    const char *content =
        "precio,cantidad\n10,2\n5,3\n9,4\n5,3\n-1,2\n";
    CHECK(write_file(csv, content), "resumen: no se pudo crear el CSV");
    const char *source =
        ".analisis resumen {\n"
        "  dataset cargar datos(\"test-language-runtime-summary-data.csv\")\n"
        "  variable precio numerica\n"
        "  variable cantidad numerica\n"
        "  .transformar dataset { #total(\"precio * cantidad\") }\n"
        "  .resumir dataset { #suma(\"total\") #media(\"total\") #conteo(\"total\") #varianza(\"total\") #desviacion_estandar(\"total\") #mediana(\"total\") #percentil(\"total,50\") }\n"
        "  .exportar { (\"test-language-runtime-summary-data.json\") }\n"
        "}\n";
    MilenaError error;
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(source,
                                     "test-language-runtime-summary-data.milena",
                                     NULL, &error) == MILENA_OK,
          error.message);
    char text[4096];
    CHECK(read_file(output, text, sizeof(text)), "resumen: no se creó el JSON");
    CHECK(strstr(text, "\"filas\": 1") != NULL &&
          strstr(text, "total_media") != NULL &&
          strstr(text, "total_varianza") != NULL &&
          strstr(text, "total_mediana") != NULL &&
          strstr(text, "total_percentil") != NULL,
          "resumen: métricas incompletas");
    remove(csv); remove(output);
    return 0;
}

static int run_stream_pipeline(void) {
    const char *csv = "test-language-runtime-stream.csv";
    const char *output = "test-language-runtime-stream.json";
    const char *content = "importe\n10\n20\n30\nno-num\n";
    CHECK(write_file(csv, content), "flujo: no se pudo crear el CSV");
    const char *source =
        ".analisis flujo_prueba {\n"
        "  dataset cargar flujo(\"test-language-runtime-stream.csv\", 2)\n"
        "  variable importe numerica\n"
        "  .resumir dataset { #suma(\"importe\") #media(\"importe\") #conteo(\"importe\") }\n"
        "  .exportar { (\"test-language-runtime-stream.json\") }\n"
        "}\n";
    MilenaError error;
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(source,
                                     "test-language-runtime-stream.milena",
                                     NULL, &error) == MILENA_OK,
          error.message);
    char text[4096];
    CHECK(read_file(output, text, sizeof(text)), "flujo: no se creó el JSON");
    CHECK(strstr(text, "\"modo\":\"flujo\"") != NULL &&
          strstr(text, "importe_suma") != NULL &&
          strstr(text, "\"tamano_lote\":2") != NULL,
          "flujo: reporte incompleto");
    remove(csv); remove(output);
    return 0;
}

static int run_human_stream_pipeline(void) {
    const char *csv = "test-language-runtime-human-stream.csv";
    const char *output = "test-language-runtime-human-stream.json";
    const char *content = "importe\n10\n20\n30\n";
    CHECK(write_file(csv, content), "flujo humano: no se pudo crear el CSV");
    const char *source =
        ".analisis ventas_grandes {\n"
        "  datos desde \"test-language-runtime-human-stream.csv\" procesar por lotes de 2 filas con registros de hasta 1 MiB con columnas de 8 con filas hasta 10 con tiempo hasta 30000 ms\n"
        "  resumir { suma de \"importe\"; media de \"importe\"; minimo de \"importe\"; maximo de \"importe\"; contar de \"importe\"; varianza de \"importe\"; desviacion_estandar de \"importe\"; }\n"
        "  guardar resultado en \"test-language-runtime-human-stream.json\"\n"
        "}\n";
    MilenaError error;
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(source,
                                     "test-language-runtime-human-stream.milena",
                                     NULL, &error) == MILENA_OK,
          error.message);
    char text[4096];
    CHECK(read_file(output, text, sizeof(text)), "flujo humano: no se creó el JSON");
    bool human_stream_ok =
          strstr(text, "\"modo\":\"flujo\"") != NULL &&
          strstr(text, "importe_suma") != NULL &&
          strstr(text, "importe_media") != NULL &&
          strstr(text, "importe_minimo") != NULL &&
          strstr(text, "importe_maximo") != NULL &&
          strstr(text, "importe_conteo") != NULL &&
          strstr(text, "importe_varianza") != NULL &&
          strstr(text, "importe_desviacion_estandar") != NULL &&
          strstr(text, "\"nombre\":\"importe_suma\",\"valores_validos\":3,\"valores_invalidos\":0,\"valor\":60") != NULL &&
          strstr(text, "\"nombre\":\"importe_media\",\"valores_validos\":3,\"valores_invalidos\":0,\"valor\":20") != NULL &&
          strstr(text, "\"nombre\":\"importe_minimo\",\"valores_validos\":3,\"valores_invalidos\":0,\"valor\":10") != NULL &&
          strstr(text, "\"nombre\":\"importe_maximo\",\"valores_validos\":3,\"valores_invalidos\":0,\"valor\":30") != NULL &&
          strstr(text, "\"nombre\":\"importe_conteo\",\"valores_validos\":3,\"valores_invalidos\":0,\"valor\":3") != NULL &&
          strstr(text, "\"nombre\":\"importe_varianza\",\"valores_validos\":3,\"valores_invalidos\":0,\"valor\":100") != NULL &&
          strstr(text, "\"nombre\":\"importe_desviacion_estandar\",\"valores_validos\":3,\"valores_invalidos\":0,\"valor\":10") != NULL &&
          strstr(text, "\"filas_por_segundo\":") != NULL &&
          strstr(text, "\"megabytes_por_segundo\":") != NULL &&
          strstr(text, "\"bytes_entrada\":") != NULL &&
          strstr(text, "\"tamano_lote\":2") != NULL &&
          strstr(text, "\"limite_registro_bytes\":1048576") != NULL &&
          strstr(text, "\"limite_columnas\":8") != NULL &&
          strstr(text, "\"limite_filas\":10") != NULL &&
          strstr(text, "\"presupuesto_tiempo_ms\":30000.000") != NULL;
    if (!human_stream_ok) fprintf(stderr, "FLUJO_HUMANO_REPORTE=[%s]\n", text);
    CHECK(human_stream_ok, "flujo humano: sintaxis o métricas no llegaron al runtime");
    remove(csv); remove(output);
    return 0;
}

static int run_grouped_human_stream_pipeline(void) {
    const char *csv = "test-language-runtime-grouped-stream.csv";
    const char *output = "test-language-runtime-grouped-stream.json";
    const char *content =
        "zona,importe,referencia\n"
        "Norte,5,r1\n"
        "Sur,1,r2\n"
        "Norte,no-num,r3\n"
        "Sur,4,\n";
    CHECK(write_file(csv, content), "agrupación de flujo: no se pudo crear el CSV");
    const char *source =
        ".analisis ventas_agrupadas {\n"
        "  datos desde \"test-language-runtime-grouped-stream.csv\" procesar por lotes de 2 filas con grupos de 4 con filas hasta 10 con tiempo hasta 30000 ms\n"
        "  agrupar por \"zona\" resumir { suma de \"importe\"; contar de \"referencia\"; }\n"
        "  guardar resultado en \"test-language-runtime-grouped-stream.json\"\n"
        "}\n";
    MilenaError error;
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(source,
        "test-language-runtime-grouped-stream.milena", NULL, &error) == MILENA_OK,
        error.message);
    char text[8192];
    CHECK(read_file(output, text, sizeof(text)),
          "agrupación de flujo: no se creó el reporte");
    const char *north = strstr(text, "\"clave\":\"Norte\"");
    const char *south = strstr(text, "\"clave\":\"Sur\"");
    CHECK(strstr(text, "\"modo\":\"flujo_agrupado\"") != NULL &&
          north != NULL && south != NULL && north < south &&
          strstr(text, "\"limite_grupos\":4") != NULL &&
          strstr(text, "\"limite_filas\":10") != NULL &&
          strstr(text, "\"presupuesto_tiempo_ms\":30000.000") != NULL &&
          strstr(text, "\"nombre\":\"importe_suma\",\"valores_validos\":1,\"valores_nulos\":0,\"valores_invalidos\":1,\"valor\":5") != NULL &&
          strstr(text, "\"nombre\":\"referencia_conteo\",\"valores_validos\":2,\"valores_nulos\":0,\"valores_invalidos\":0,\"valor\":2") != NULL &&
          strstr(text, "\"nombre\":\"referencia_conteo\",\"valores_validos\":1,\"valores_nulos\":1,\"valores_invalidos\":0,\"valor\":1") != NULL,
          "agrupación de flujo: AST, orden o semántica de valores inválidos incorrectos");
    remove(csv);
    remove(output);
    return 0;
}

static int run_int64_grouped_spill_adapter(void) {
    MilenaError error;
    milena_error_clear(&error);
    const size_t row_count = 14;
    const size_t shape[] = {row_count};
    const char *keys[] = {"A", "A", "k00", "k01", "k02", "k03", "k04",
                          "k05", "k06", "k07", "k08", "A", "B", "C"};
    const int64_t values[] = {INT64_C(9007199254740993), 1, 10, 11, 12, 13,
                              14, 15, 16, 17, 18, 1, INT64_MIN, 0};
    bool validity[row_count];
    for (size_t i = 0; i < row_count; ++i) validity[i] = true;
    validity[row_count - 1] = false;
    MilenaArray input_values = {0};
    CHECK(milena_array_from_i64(&input_values, 1, shape, values, &error) == MILENA_OK,
          error.message);
    MilenaTable input = {0}, output = {0};
    milena_table_init(&input);
    CHECK(milena_table_add_string_column_copy(&input, "group", keys, row_count,
                                               NULL, &error) == MILENA_OK,
          error.message);
    CHECK(milena_table_add_column_copy(&input, "value", &input_values,
                                       validity, &error) == MILENA_OK,
          error.message);
    const char *scratch = "test-language-runtime-int64-grouped.spill";
    ASTNode policy = {0};
    policy.type = AST_AGRUPACION_SPILL;
    policy.value = (char *)scratch;
    policy.group_memory_budget_bytes = 4096;
    policy.group_spill_quota_bytes = 1024 * 1024;
    policy.group_max_key_bytes = 128;
    policy.group_max_output_groups = 32;
    policy.group_max_runs = 32;
    MilenaAggregateSpec spec = {"value", MILENA_AGG_SUM, NULL};
    CHECK(milena_language_group_by_spill(&output, &input, "group", &spec,
                                          &policy, &error) == MILENA_OK,
          error.message);
    CHECK(output.row_count == 12, "spill INT64: cardinalidad de salida inesperada");
    const MilenaTableColumn *sums = milena_table_column(&output, 1);
    CHECK(sums != NULL && sums->values.dtype == MILENA_DTYPE_INT64,
          "spill INT64: suma no conservó el tipo INT64");
    const int64_t *sum_values = milena_array_const_data(&sums->values);
    bool saw_large = false, saw_min = false, saw_null_group = false;
    const MilenaTableColumn *out_keys = milena_table_column(&output, 0);
    for (size_t i = 0; i < output.row_count; ++i) {
        const char *key = out_keys->strings[i];
        if (strcmp(key, "A") == 0) {
            saw_large = true;
            CHECK(sum_values[i] == INT64_C(9007199254740995),
                  "spill INT64: suma perdio precisión por encima de 2^53");
        } else if (strcmp(key, "B") == 0) {
            saw_min = true;
            CHECK(sum_values[i] == INT64_MIN, "spill INT64: INT64_MIN alterado");
        } else if (strcmp(key, "C") == 0) {
            saw_null_group = true;
            CHECK(!sums->validity[i], "spill INT64: grupo nulo se volvió valor válido");
        }
    }
    CHECK(saw_large && saw_min && saw_null_group,
          "spill INT64: faltó un grupo esperado");
    CHECK(fopen(scratch, "rb") == NULL,
          "spill INT64: no se limpió el temporal propiedad de la operación");
    milena_table_destroy(&output);
    milena_table_destroy(&input);
    milena_array_release(&input_values);
    return 0;
}

int main(void) {
    CHECK(run_arrays() == 0, "falló la fase de arrays");
    CHECK(run_dataset_pipeline() == 0, "falló la fase de datasets");
    CHECK(run_typed_data_hir_runtime() == 0, "falló la fase de HIR de datos");
    CHECK(run_inference_pipeline() == 0, "falló la fase de inferencia");
    CHECK(run_summary_pipeline() == 0, "falló la fase de resumen");
    CHECK(run_stream_pipeline() == 0, "falló la fase de flujo");
    CHECK(run_human_stream_pipeline() == 0, "falló la fase de flujo humano");
    CHECK(run_grouped_human_stream_pipeline() == 0,
          "falló la fase de agrupación de flujo humano");
    CHECK(run_int64_grouped_spill_adapter() == 0,
          "falló la fase canónica de spill agrupado INT64");
    puts("language runtime: parser + AST + arrays + datasets + SST + finanzas + flujo agrupado OK");
    return 0;
}
