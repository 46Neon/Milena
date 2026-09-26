#include "language_runtime.h"
#include "dataset.h"
#include "canonical_compiler.h"
#include "language_grouped_spill.h"
#include "language_semantic.h"
#include "query_plan.h"
#include "parser.h"
#include "lexer.h"

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

static bool format_materialized_memory_option(size_t bytes, char *buffer,
                                              size_t buffer_size) {
    if (!buffer || buffer_size == 0 || bytes == 0) return false;
    double mib = (double)bytes / (1024.0 * 1024.0);
    int length = snprintf(buffer, buffer_size,
                          "con memoria hasta %.20f MiB", mib);
    return length > 0 && (size_t)length < buffer_size;
}

static bool write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    size_t length = strlen(content);
    bool ok = fwrite(content, 1, length, file) == length && fclose(file) == 0;
    if (!ok) fclose(file);
    return ok;
}

static int expect_materialized_limit_failure(const char *csv_path,
    const char *csv_content, const char *source_options,
    const char *output_path, const char *program_path,
    const char *expected_error_fragment) {
    CHECK(write_file(csv_path, csv_content), "límites materializados: no se pudo crear el CSV");
    CHECK(write_file(output_path, "keep-prior-destination\n"),
          "límites materializados: no se pudo preparar el destino previo");
    char source[2048];
    int length = snprintf(source, sizeof(source),
        ".analisis limites_materializados {\n"
        "  variable importe numerica\n"
        "  dataset cargar datos(\"%s\") %s\n"
        "  .resumir dataset { #suma(\"importe\"); }\n"
        "  .exportar { (\"%s\") }\n"
        "}\n", csv_path, source_options, output_path);
    CHECK(length > 0 && (size_t)length < sizeof(source),
          "límites materializados: la fuente del programa se truncó");
    MilenaError error;
    milena_error_clear(&error);
    MilenaStatus status = milena_run_dataset_program(source, program_path, NULL, &error);
    CHECK(status == MILENA_ERR_DATA,
          error.message[0] ? error.message : "límites materializados: el límite debía fallar");
    CHECK(expected_error_fragment == NULL ||
          strstr(error.message, expected_error_fragment) != NULL,
          "límites materializados: falló un límite distinto del esperado");
    char previous[128];
    CHECK(read_file(output_path, previous, sizeof(previous)) &&
          strcmp(previous, "keep-prior-destination\n") == 0,
          "límites materializados: un fallo alteró el destino previo");
    remove(csv_path);
    remove(output_path);
    return 0;
}

static int run_dataset_raw_input_byte_limit(void) {
    const char *path = "test-materialized-total-bytes.csv";
    CHECK(write_file(path, "h\r\nx\r\n"),
          "límite total de bytes: no se pudo crear CSV CRLF");
    Dataset dataset;
    dataset_init(&dataset);
    DatasetLoadLimits limits = dataset_default_load_limits();
    CHECK(limits.max_input_bytes == 0u && limits.max_memory_bytes == 0u,
          "límite total de bytes: los defaults de entrada y memoria deben permanecer sin cap explícito");
    limits.max_memory_bytes = 1024u * 1024u;
    limits.max_input_bytes = 6u;
    MilenaError error;
    milena_error_clear(&error);
    CHECK(dataset_load_csv_with_resource_limits(&dataset, path, ',', &limits,
          &error) == MILENA_OK,
          error.message[0] ? error.message : "el CSV exacto debía caber en el límite total");
    CHECK(dataset.column_count == 1u && dataset.row_count == 1u &&
          strcmp(dataset.headers[0], "h") == 0 &&
          strcmp(dataset.rows[0][0], "x") == 0,
          "límite total de bytes: se alteró la lectura del CSV CRLF exacto");
    char ***prior_rows = dataset.rows;
    char **prior_headers = dataset.headers;
    char *prior_filename = dataset.filename;
    limits.max_input_bytes = 5u;
    milena_error_clear(&error);
    CHECK(dataset_load_csv_with_resource_limits(&dataset, path, ',', &limits,
          &error) == MILENA_ERR_DATA && strstr(error.message, "bytes de entrada") != NULL,
          "límite total de bytes: cap+1 físico debía fallar explícitamente");
    CHECK(dataset.rows == prior_rows && dataset.headers == prior_headers &&
          dataset.filename == prior_filename && dataset.row_count == 1u &&
          strcmp(dataset.rows[0][0], "x") == 0,
          "límite total de bytes: un fallo sustituyó el Dataset previo");

    /* The header and its CRLF separator are charged before data records. */
    limits.max_input_bytes = 2u;
    milena_error_clear(&error);
    CHECK(dataset_load_csv_with_resource_limits(&dataset, path, ',', &limits,
          &error) == MILENA_ERR_DATA && strstr(error.message, "bytes de entrada") != NULL,
          "límite total de bytes: el encabezado/CRLF debe contar en el cap");
    CHECK(dataset.rows == prior_rows && dataset.headers == prior_headers &&
          dataset.filename == prior_filename,
          "límite total de bytes: fallo durante encabezado alteró el Dataset previo");

    /* A non-LF lookahead after a bare CR is charged once, then consumed from
     * the pending-byte slot rather than counted a second time. */
    CHECK(write_file(path, "h\rx\r"),
          "límite total de bytes: no se pudo crear CSV con CR aislados");
    limits.max_input_bytes = 4u;
    milena_error_clear(&error);
    CHECK(dataset_load_csv_with_resource_limits(&dataset, path, ',', &limits,
          &error) == MILENA_OK && dataset.row_count == 1u &&
          strcmp(dataset.rows[0][0], "x") == 0,
          "límite total de bytes: el lookahead CR sin LF se contó dos veces");
    prior_rows = dataset.rows;
    prior_headers = dataset.headers;
    prior_filename = dataset.filename;
    limits.max_input_bytes = 3u;
    milena_error_clear(&error);
    CHECK(dataset_load_csv_with_resource_limits(&dataset, path, ',', &limits,
          &error) == MILENA_ERR_DATA,
          "límite total de bytes: faltó rechazar cap+1 tras lookahead CR");
    CHECK(dataset.rows == prior_rows && dataset.headers == prior_headers &&
          dataset.filename == prior_filename && dataset.row_count == 1u,
          "límite total de bytes: el lookahead alteró el Dataset previo al fallar");
    dataset_destroy(&dataset);
    remove(path);
    return 0;
}

static int run_materialized_source_limits(void) {
    CHECK(run_dataset_raw_input_byte_limit() == 0,
          "falló el límite total de bytes de la API CSV materializada");
    MilenaError error;
    const char *typed_source =
        ".analisis limites_hir {\n"
        "  variable importe numerica\n"
        "  dataset cargar datos(\"missing-materialized-limits.csv\") con filas hasta 1 con columnas de 2 con registros de hasta 0.0048828125 MiB con tiempo hasta 30000 ms con memoria hasta 0.00000095367431640625 MiB con entrada hasta 7 MiB\n"
        "  .resumir dataset { #suma(\"importe\"); }\n"
        "  .exportar { (\"unused-limits.json\") }\n"
        "}\n";
    MilenaCanonicalProgram canonical;
    milena_canonical_program_init(&canonical);
    milena_error_clear(&error);
    CHECK(milena_canonical_program_parse(&canonical, typed_source, &error) == MILENA_OK,
          error.message);
    const ASTNode *analysis = canonical.ast && canonical.ast->child_count == 1
        ? canonical.ast->children[0] : NULL;
    const ASTNode *ast_load = NULL;
    if (analysis && analysis->type == AST_BLOQUE_ANALISIS) {
        for (size_t i = 0; i < analysis->child_count; ++i) {
            if (analysis->children[i] &&
                analysis->children[i]->type == AST_LLAMADA_CARGAR) {
                ast_load = analysis->children[i];
                break;
            }
        }
    }
    CHECK(ast_load != NULL && ast_load->source_max_rows == 1u &&
          ast_load->source_max_columns == 2u &&
          ast_load->source_max_record_bytes == 5120u &&
          ast_load->source_max_elapsed_milliseconds == 30000.0 &&
          ast_load->source_max_memory_bytes == 1u &&
          ast_load->source_max_input_bytes == 7u * 1024u * 1024u,
          "límites materializados: el parser no conservó las seis políticas en el AST");
    CHECK(canonical.data_hir != NULL &&
          canonical.data_hir->source.max_rows == ast_load->source_max_rows &&
          canonical.data_hir->source.max_columns == ast_load->source_max_columns &&
          canonical.data_hir->source.max_record_bytes == ast_load->source_max_record_bytes &&
          canonical.data_hir->source.max_elapsed_milliseconds ==
              ast_load->source_max_elapsed_milliseconds &&
          canonical.data_hir->source.max_memory_bytes ==
              ast_load->source_max_memory_bytes &&
          canonical.data_hir->source.max_input_bytes ==
              ast_load->source_max_input_bytes,
          "límites materializados: HIR no propagó las seis políticas del AST");
    milena_canonical_program_release(&canonical);

    CHECK(expect_materialized_limit_failure(
        "test-materialized-row-limit.csv", "importe\nbroken,row\n1\n",
        "con filas hasta 1", "test-materialized-row-limit.json",
        "test-materialized-row-limit.milena", "filas") == 0,
        "límite de filas materializado no se aplicó antes de publicar");
    CHECK(expect_materialized_limit_failure(
        "test-materialized-column-limit.csv", "\"clave,extendida\",importe\nA,1\n",
        "con columnas de 1", "test-materialized-column-limit.json",
        "test-materialized-column-limit.milena", "columnas") == 0,
        "límite de columnas materializado no se aplicó al encabezado CSV");

    size_t record_length = 6000u;
    char *record_content = (char *)malloc(record_length + 16u);
    CHECK(record_content != NULL, "límite de registro: sin memoria para la prueba");
    memcpy(record_content, "importe\n", 8u);
    memset(record_content + 8u, 'x', record_length);
    record_content[8u + record_length] = '\n';
    record_content[9u + record_length] = '\0';
    CHECK(expect_materialized_limit_failure(
        "test-materialized-record-limit.csv", record_content,
        "con registros de hasta 0.0048828125 MiB",
        "test-materialized-record-limit.json",
        "test-materialized-record-limit.milena", "registro") == 0,
        "límite de registro materializado no se aplicó durante la lectura");
    free(record_content);

    size_t slow_record_length = 8u * 1024u * 1024u;
    char *slow_content = (char *)malloc(slow_record_length + 16u);
    CHECK(slow_content != NULL, "límite de tiempo: sin memoria para la prueba");
    memcpy(slow_content, "importe\n", 8u);
    memset(slow_content + 8u, 'x', slow_record_length);
    slow_content[8u + slow_record_length] = '\n';
    slow_content[9u + slow_record_length] = '\0';
    CHECK(expect_materialized_limit_failure(
        "test-materialized-time-limit.csv", slow_content,
        "con registros de hasta 64 MiB con tiempo hasta 1 ms",
        "test-materialized-time-limit.json",
        "test-materialized-time-limit.milena", "tiempo") == 0,
        "límite de tiempo materializado no se aplicó durante la lectura");
    free(slow_content);

    const size_t input_cap = 1024u * 1024u;
    const char *input_header = "importe\n";
    size_t input_header_length = strlen(input_header);
    char *over_input = (char *)malloc(input_cap + 2u);
    CHECK(over_input != NULL, "límite total de bytes: sin memoria para fixture");
    memcpy(over_input, input_header, input_header_length);
    memset(over_input + input_header_length, '1',
           input_cap + 1u - input_header_length);
    over_input[input_cap + 1u] = '\0';
    CHECK(expect_materialized_limit_failure(
        "test-materialized-input-limit.csv", over_input,
        "con memoria hasta 1 MiB con entrada hasta 1 MiB", "test-materialized-input-limit.json",
        "test-materialized-input-limit.milena", "bytes de entrada") == 0,
        "límite total de bytes materializado no preservó el reporte previo");
    free(over_input);

    const char *memory_csv_path = "test-materialized-memory.csv";
    const char *memory_csv_content = "importe\n1\n";
    const char *memory_output_path = "test-materialized-memory.json";
    const char *memory_program_path = "test-materialized-memory.milena";
    /* Exact requested capacities on both 32- and 64-bit targets: filename,
     * 8-slot header/cell vectors, 64-row vector, and two 32-byte strings. */
    size_t exact_memory_bytes = strlen(memory_csv_path) + 1u +
        80u * sizeof(char *) + 64u;
    char memory_option[128];
    CHECK(format_materialized_memory_option(exact_memory_bytes,
          memory_option, sizeof(memory_option)),
          "límite de memoria: no se pudo formatear el presupuesto exacto");
    CHECK(write_file(memory_csv_path, memory_csv_content),
          "límite de memoria: no se pudo crear el CSV de frontera");
    char memory_source[2048];
    int memory_source_length = snprintf(memory_source, sizeof(memory_source),
        ".analisis limite_memoria {\n"
        "  variable importe numerica\n"
        "  dataset cargar datos(\"%s\") %s\n"
        "  .resumir dataset { #suma(\"importe\"); }\n"
        "  .exportar { (\"%s\") }\n"
        "}\n", memory_csv_path, memory_option, memory_output_path);
    CHECK(memory_source_length > 0 &&
          (size_t)memory_source_length < sizeof(memory_source),
          "límite de memoria: la fuente de frontera se truncó");
    remove(memory_output_path);
    MilenaError memory_error;
    milena_error_clear(&memory_error);
    CHECK(milena_run_dataset_program(memory_source, memory_program_path,
          NULL, &memory_error) == MILENA_OK,
          memory_error.message[0] ? memory_error.message :
          "límite de memoria: el presupuesto exacto debía permitir la carga");
    char memory_report[2048];
    CHECK(read_file(memory_output_path, memory_report, sizeof(memory_report)) &&
          memory_report[0] != '\0',
          "límite de memoria: no se publicó el reporte en la frontera exacta");
    remove(memory_csv_path);
    remove(memory_output_path);

    CHECK(format_materialized_memory_option(exact_memory_bytes - 1u,
          memory_option, sizeof(memory_option)),
          "límite de memoria: no se pudo formatear el caso un byte menor");
    CHECK(expect_materialized_limit_failure(memory_csv_path, memory_csv_content,
        memory_option, memory_output_path, memory_program_path,
        "memoria retenida") == 0,
        "límite de memoria: el caso un byte menor no rechazó la asignación");

    CHECK(format_materialized_memory_option(strlen(memory_csv_path) + 1u,
          memory_option, sizeof(memory_option)),
          "límite de memoria: no se pudo preparar el rechazo previo a asignar");
    CHECK(expect_materialized_limit_failure(memory_csv_path, memory_csv_content,
        memory_option, memory_output_path, memory_program_path,
        "memoria retenida") == 0,
        "límite de memoria: una asignación que excedía el cap no se rechazó");

    const char *invalid_input_clauses[] = {
        "con entrada hasta 0 MiB",
        "con entrada hasta 1.5 MiB",
        "con entrada hasta 18446744073709551616 MiB",
        "con entrada hasta 1",
        "con entrada hasta 1 MiB con entrada hasta 2 MiB"
    };
    for (size_t i = 0; i < sizeof(invalid_input_clauses) /
                            sizeof(invalid_input_clauses[0]); ++i) {
        char invalid_input_source[1024];
        int written = snprintf(invalid_input_source, sizeof(invalid_input_source),
            ".analisis limite_entrada_invalido { dataset cargar datos(\"missing.csv\") %s }",
            invalid_input_clauses[i]);
        CHECK(written > 0 && (size_t)written < sizeof(invalid_input_source),
              "límite total de bytes: no se pudo preparar cláusula inválida");
        milena_error_clear(&error);
        CHECK(milena_run_dataset_program(invalid_input_source, NULL, NULL,
              &error) == MILENA_ERR_PARSE,
              "límite total de bytes: el parser aceptó valor cero, fraccionario, desbordado, sin unidad o repetido");
    }

    const char *invalid_source =
        ".analisis limite_cero {\n"
        "  dataset cargar datos(\"test-materialized-row-limit.csv\") con filas hasta 0\n"
        "}\n";
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(invalid_source,
        "test-materialized-invalid-limit.milena", NULL, &error) == MILENA_ERR_PARSE,
        "límites materializados: el valor cero debió rechazarse por el parser");
    const char *duplicate_source =
        ".analisis limite_repetido {\n"
        "  dataset cargar datos(\"missing-materialized-limits.csv\") con filas hasta 2 con filas hasta 3\n"
        "}\n";
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(duplicate_source,
        "test-materialized-duplicate-limit.milena", NULL, &error) == MILENA_ERR_PARSE,
        "límites materializados: las declaraciones repetidas debieron rechazarse");
    const char *zero_memory_source =
        ".analisis memoria_cero {\n"
        "  dataset cargar datos(\"missing-materialized-limits.csv\") con memoria hasta 0 MiB\n"
        "}\n";
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(zero_memory_source,
        "test-materialized-zero-memory.milena", NULL, &error) == MILENA_ERR_PARSE,
        "límite de memoria materializado: el cero debió rechazarse");
    const char *duplicate_memory_source =
        ".analisis memoria_repetida {\n"
        "  dataset cargar datos(\"missing-materialized-limits.csv\") con memoria hasta 1 MiB con memoria hasta 2 MiB\n"
        "}\n";
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(duplicate_memory_source,
        "test-materialized-duplicate-memory.milena", NULL, &error) == MILENA_ERR_PARSE,
        "límite de memoria materializado: la repetición debió rechazarse");
    const char *overflow_memory_source =
        ".analisis memoria_fuera_de_rango {\n"
        "  dataset cargar datos(\"missing-materialized-limits.csv\") con memoria hasta 18446744073709551616 MiB\n"
        "}\n";
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(overflow_memory_source,
        "test-materialized-memory-overflow.milena", NULL, &error) == MILENA_ERR_PARSE,
        "límite de memoria materializado: el desbordamiento MiB-a-bytes debía rechazarse antes del cast");
    return 0;
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

static int run_stream_filter_pipeline(void) {
    const char *csv = "test-language-runtime-filter.csv";
    const char *duplicate = "test-language-runtime-filter-duplicate.csv";
    const char *output = "test-language-runtime-filter.json";
    const char *unfiltered = "test-language-runtime-filter-unfiltered.json";
    const char *content =
        "zona,estado,importe\n"
        "\"Norte, Este\",ok,5\n"
        "Sur,ok,10\n"
        "\"Norte, Este\",ok,7\n"
        "Sur,no,4\n";
    CHECK(write_file(csv, content), "filtro: no se pudo crear el CSV");
    const char *filtered_source =
        ".analisis filtro_agrupado {\n"
        "  variable zona texto\n"
        "  variable estado texto\n"
        "  variable importe numerica\n"
        "  datos desde \"test-language-runtime-filter.csv\" con grupos de 10 con filas hasta 100 con tiempo hasta 30000 ms\n"
        "  filtrar \"zona\" == \"Norte, Este\";\n"
        "  agrupar por \"zona\" resumir { suma de \"importe\"; }\n"
        "  guardar resultado en \"test-language-runtime-filter.json\"\n"
        "}\n";
    MilenaError error;
    milena_error_clear(&error);
    CHECK(milena_run_dataset_program(filtered_source,
        "test-language-runtime-filter.milena", NULL, &error) == MILENA_OK,
        error.message);
    char text[8192];
    CHECK(read_file(output, text, sizeof(text)), "filtro: no se creó el reporte");
    CHECK(strstr(text, "\"clave\":\"Norte, Este\"") != NULL &&
          strstr(text, "\"valor\":12") != NULL &&
          strstr(text, "\"clave\":\"Sur\"") == NULL,
          "filtro: el CSV citado no llegó al agregado agrupado como igualdad exacta");

    const char *unfiltered_source =
        ".analisis sin_filtro {\n"
        "  variable zona texto\n"
        "  variable importe numerica\n"
        "  datos desde \"test-language-runtime-filter.csv\" con grupos de 10 con filas hasta 100 con tiempo hasta 30000 ms\n"
        "  agrupar por \"zona\" resumir { suma de \"importe\"; }\n"
        "  guardar resultado en \"test-language-runtime-filter-unfiltered.json\"\n"
        "}\n";
    CHECK(milena_run_dataset_program(unfiltered_source,
        "test-language-runtime-filter.milena", NULL, &error) == MILENA_OK,
        error.message);
    CHECK(read_file(unfiltered, text, sizeof(text)) &&
          strstr(text, "\"clave\":\"Norte, Este\"") != NULL &&
          strstr(text, "\"valor\":12") != NULL &&
          strstr(text, "\"clave\":\"Sur\"") != NULL &&
          strstr(text, "\"valor\":14") != NULL,
          "filtro: resultado filtrado no coincide con agregado sin filtro de la clave seleccionada");

    const char *numeric_csv = "test-language-runtime-filter-numeric.csv";
    const char *numeric_output = "test-language-runtime-filter-numeric.json";
    const char *numeric_content =
        "importe,aporte\n"
        "10,1\n"
        "10.01,2\n"
        "11,3\n"
        ",100\n"
        "no-numerico,100\n"
        "10.0,4\n"
        "nan,99\n"
        "12x,99\n";
    CHECK(write_file(numeric_csv, numeric_content),
          "filtro numérico: no se pudo crear el CSV");
    const char *numeric_source =
        ".analisis filtro_numerico {\n"
        "  variable importe numerica\n"
        "  variable aporte numerica\n"
        "  datos desde \"test-language-runtime-filter-numeric.csv\" con filas hasta 100\n"
        "  filtrar \"importe\" > 10;\n"
        "  resumir { suma de \"aporte\"; }\n"
        "  guardar resultado en \"test-language-runtime-filter-numeric.json\"\n"
        "}\n";
    CHECK(milena_run_dataset_program(numeric_source,
        "test-language-runtime-filter.milena", NULL, &error) == MILENA_OK,
        error.message);
    CHECK(read_file(numeric_output, text, sizeof(text)) &&
          strstr(text, "\"valor\":5") != NULL &&
          strstr(text, "\"valores_validos\":2") != NULL,
          "filtro numérico: > debe excluir límite, nulos, texto inválido y NaN");

    const char *numeric_missing_csv = "test-language-runtime-filter-numeric-missing.csv";
    CHECK(write_file(numeric_missing_csv, "aporte\n1\n"),
          "filtro numérico: no se pudo crear CSV sin columna de filtro");
    const char *numeric_missing_source =
        ".analisis filtro_numerico_sin_columna {\n"
        "  variable importe numerica\n"
        "  variable aporte numerica\n"
        "  datos desde \"test-language-runtime-filter-numeric-missing.csv\"\n"
        "  filtrar \"importe\" > 10;\n"
        "  resumir { suma de \"aporte\"; }\n"
        "}\n";
    CHECK(milena_run_dataset_program(numeric_missing_source,
        "test-language-runtime-filter.milena", NULL, &error) == MILENA_ERR_DATA,
        "filtro numérico: una columna tipada ausente de la cabecera debe rechazarse");

    const char *wrong_type_source =
        ".analisis filtro_tipo_incorrecto {\n"
        "  variable importe texto\n"
        "  variable aporte numerica\n"
        "  datos desde \"test-language-runtime-filter-numeric.csv\"\n"
        "  filtrar \"importe\" > 10;\n"
        "  resumir { suma de \"aporte\"; }\n"
        "}\n";
    CHECK(milena_run_dataset_program(wrong_type_source,
        "test-language-runtime-filter.milena", NULL, &error) != MILENA_OK,
        "filtro numérico: columna no numérica debe rechazarse semánticamente");

    const char *undeclared_source =
        ".analisis filtro_sin_declaracion {\n"
        "  variable aporte numerica\n"
        "  datos desde \"test-language-runtime-filter-numeric.csv\"\n"
        "  filtrar \"importe\" > 10;\n"
        "  resumir { suma de \"aporte\"; }\n"
        "}\n";
    CHECK(milena_run_dataset_program(undeclared_source,
        "test-language-runtime-filter.milena", NULL, &error) != MILENA_OK,
        "filtro numérico: columna sin declaración tipada debe rechazarse");

    const char *global_output = "test-language-runtime-filter-global.json";
    const char *global_source =
        ".analisis filtro_global {\n"
        "  variable estado texto\n"
        "  variable importe numerica\n"
        "  datos desde \"test-language-runtime-filter.csv\" con filas hasta 100 con tiempo hasta 30000 ms\n"
        "  filtrar \"estado\" == \"ok\";\n"
        "  resumir { suma de \"importe\"; }\n"
        "  guardar resultado en \"test-language-runtime-filter-global.json\"\n"
        "}\n";
    CHECK(milena_run_dataset_program(global_source,
        "test-language-runtime-filter.milena", NULL, &error) == MILENA_OK,
        error.message);
    CHECK(read_file(global_output, text, sizeof(text)) &&
          strstr(text, "\"valor\":22") != NULL,
          "filtro: igualdad exacta debe preceder también al resumen global");

    const char *none_source =
        ".analisis filtro_sin_coincidencias {\n"
        "  variable zona texto\n"
        "  variable importe numerica\n"
        "  datos desde \"test-language-runtime-filter.csv\" con grupos de 10 con filas hasta 100 con tiempo hasta 30000 ms\n"
        "  filtrar \"zona\" == \"No existe\";\n"
        "  agrupar por \"zona\" resumir { suma de \"importe\"; }\n"
        "  guardar resultado en \"test-language-runtime-filter.json\"\n"
        "}\n";
    CHECK(milena_run_dataset_program(none_source,
        "test-language-runtime-filter.milena", NULL, &error) == MILENA_OK,
        error.message);
    CHECK(read_file(output, text, sizeof(text)) && strstr(text, "\"grupos\":0") != NULL,
          "filtro: un predicado sin coincidencias debe producir agrupación vacía");

    const char *missing_source =
        ".analisis filtro_columna_ausente {\n"
        "  variable no_existe texto\n"
        "  variable importe numerica\n"
        "  datos desde \"test-language-runtime-filter.csv\"\n"
        "  filtrar \"no_existe\" == \"x\";\n"
        "  resumir { suma de \"importe\"; }\n"
        "}\n";
    CHECK(milena_run_dataset_program(missing_source,
        "test-language-runtime-filter.milena", NULL, &error) == MILENA_ERR_DATA,
        "filtro: columna declarada pero ausente en el CSV debe rechazarse");

    CHECK(write_file(duplicate, "zona,zona,importe\na,a,1\n"),
          "filtro: no se pudo crear CSV con cabecera duplicada");
    const char *duplicate_source =
        ".analisis filtro_cabecera_duplicada {\n"
        "  variable zona texto\n"
        "  variable importe numerica\n"
        "  datos desde \"test-language-runtime-filter-duplicate.csv\"\n"
        "  filtrar \"zona\" == \"a\";\n"
        "  resumir { suma de \"importe\"; }\n"
        "}\n";
    CHECK(milena_run_dataset_program(duplicate_source,
        "test-language-runtime-filter.milena", NULL, &error) == MILENA_ERR_DATA,
        "filtro: cabecera duplicada debe rechazarse");

    const char *invalid_source =
        ".analisis filtro_no_soportado {\n"
        "  variable importe numerica\n"
        "  datos desde \"test-language-runtime-filter.csv\"\n"
        "  filtrar \"importe\" >= 3;\n"
        "  resumir { suma de \"importe\"; }\n"
        "}\n";
    CHECK(milena_run_dataset_program(invalid_source,
        "test-language-runtime-filter.milena", NULL, &error) == MILENA_ERR_PARSE,
        "filtro: operadores y expresiones no soportados deben fallar antes de ejecutar");
    remove(csv); remove(duplicate); remove(output); remove(unfiltered); remove(global_output);
    remove(numeric_csv); remove(numeric_output); remove(numeric_missing_csv);
    return 0;
}
static int run_typed_sql_semantics(void) {
    const char *missing_db = "test-sql-typed-semantic-missing.db";
    const char *valid =
        "sql desde \"test-sql-typed-semantic-missing.db\" {\n"
        "  tabla personas (id entero, nombre texto, activo booleano);\n"
        "  seleccionar id, nombre de personas donde id = 7;\n"
        "}";
    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, valid);
    parser_init(&parser, &lexer);
    ASTNode *program = parser_parse(&parser);
    CHECK(program != NULL && !parser.has_error,
          "SQL tipado: el parser no construyó el AST válido");
    parser_release(&parser);
    CHECK(program->child_count == 1 &&
          program->children[0]->type == AST_SQL_PROGRAM,
          "SQL tipado: raíz AST incorrecta");
    const ASTNode *sql = program->children[0];
    CHECK(sql->child_count == 2 &&
          sql->children[0]->type == AST_SQL_TABLE_SCHEMA &&
          sql->children[0]->child_count == 3 &&
          sql->children[0]->children[0]->sql_type == AST_SQL_TYPE_INTEGER &&
          sql->children[0]->children[1]->sql_type == AST_SQL_TYPE_TEXT &&
          sql->children[0]->children[2]->sql_type == AST_SQL_TYPE_BOOLEAN,
          "SQL tipado: AST del esquema no preservó nombres/tipos tipados");
    const ASTNode *select = sql->children[1];
    CHECK(select->type == AST_SQL_TYPED_SELECT && select->child_count == 3 &&
          select->children[0]->type == AST_SQL_TABLE_REFERENCE &&
          select->children[1]->type == AST_SQL_PROJECTION_LIST &&
          select->children[1]->child_count == 2 &&
          select->children[2]->type == AST_SQL_FILTER &&
          select->children[2]->children[1]->sql_operator == AST_SQL_OPERATOR_EQUAL &&
          select->children[2]->children[2]->sql_type == AST_SQL_TYPE_INTEGER,
          "SQL tipado: AST de tabla, proyección, operador o parámetro incorrecto");
    MilenaError error;
    CHECK(milena_validate_ast(program, &error) == MILENA_OK,
          "SQL tipado: la validación AST general rechazó el árbol");
    CHECK(milena_sql_semantic_validate(sql, &error) == MILENA_OK,
          "SQL tipado: la validación semántica rechazó el esquema y SELECT válidos");
    MilenaSqlExecutionPlan plan = {0};
    CHECK(milena_sql_execution_plan_build(sql, &plan, &error) == MILENA_OK,
          "SQL tipado: no se construyó el plan validado");
    CHECK(plan.operation_count == 2 &&
          plan.operations[0].kind == MILENA_SQL_PLAN_SCHEMA &&
          plan.operations[1].kind == MILENA_SQL_PLAN_TYPED_SELECT &&
          strcmp(plan.operations[1].statement,
                 "SELECT \"id\", \"nombre\" FROM \"personas\" WHERE \"id\" COLLATE BINARY = ?") == 0 &&
          plan.operations[1].parameter_count == 1 &&
          plan.operations[1].parameters[0].kind == MILENA_SQL_PLAN_INT64 &&
          plan.operations[1].parameters[0].value.i64 == 7 &&
          plan.operations[1].projections[0].type == AST_SQL_TYPE_INTEGER,
          "SQL tipado: plan no contiene SQL con placeholder/AST y binding tipados");
    milena_sql_execution_plan_destroy(&plan);
    ast_destroy(program);

    struct {
        const char *description;
        const char *statement;
        MilenaStatus expected;
    } invalid[] = {
        {"tabla desconocida", "seleccionar id de fantasma donde id = 7;", MILENA_ERR_TYPE},
        {"proyección desconocida", "seleccionar ausente de personas donde id = 7;", MILENA_ERR_TYPE},
        {"filtro desconocido", "seleccionar id de personas donde ausente = 7;", MILENA_ERR_TYPE},
        {"tipo de parámetro incompatible", "seleccionar id de personas donde id = \"7\";", MILENA_ERR_TYPE},
        {"operador no admitido", "seleccionar id de personas donde id > 7;", MILENA_ERR_UNSUPPORTED}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char source[1024];
        int written = snprintf(source, sizeof(source),
            "sql desde \"%s\" { tabla personas (id entero); %s }",
            missing_db, invalid[i].statement);
        CHECK(written > 0 && (size_t)written < sizeof(source),
              "SQL tipado: no se pudo preparar el caso semántico inválido");
        (void)remove(missing_db);
        MilenaStatus status = milena_run_dataset_program(source, NULL, NULL,
                                                         &error);
        CHECK(status == invalid[i].expected,
              invalid[i].description);
        FILE *created = fopen(missing_db, "rb");
        CHECK(created == NULL,
              "SQL tipado: un error semántico abrió o creó la base de datos");
        if (created) fclose(created);
    }
    (void)remove(missing_db);
    return 0;
}

int main(void) {
    CHECK(run_typed_sql_semantics() == 0,
          "falló la fase de AST y semántica SQL tipados");
    CHECK(run_arrays() == 0, "falló la fase de arrays");
    CHECK(run_materialized_source_limits() == 0,
          "falló la fase de límites materializados");
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

    CHECK(run_stream_filter_pipeline() == 0,
          "falló la fase de filtro de flujo canónico");
    puts("language runtime: parser + AST + arrays + datasets + SST + finanzas + flujo agrupado + filtro OK");
    return 0;
}
