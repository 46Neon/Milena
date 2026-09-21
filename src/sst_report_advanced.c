#include "sst_report.h"

MilenaStatus sst_report_write_advanced_json(
    const char *filename,
    const SstAdvancedStats *advanced,
    const SstPoissonInterval *poisson,
    const SstRiskMeasure *risk,
    const SstMannWhitneyResult *mann_whitney,
    const SstWilcoxonResult *wilcoxon,
    const SstChiSquareResult *chi_square,
    MilenaError *error
) {
    if (!filename || !advanced) return MILENA_ERR_ARGUMENT;
    FILE *out = fopen(filename, "wb");
    if (!out) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "No se pudo abrir reporte avanzado SST");
        return MILENA_ERR_IO;
    }
    fprintf(out, "{\n  \"analisis\": \"sst_investigacion\",\n");
    fprintf(out, "  \"estadistica_avanzada\": {\"n\": %zu, \"invalidos\": %zu, "
                 "\"media\": %.10g, \"varianza\": %.10g, \"desviacion\": %.10g, "
                 "\"cv\": %.10g, \"asimetria\": %.10g, \"kurtosis_exceso\": %.10g, "
                 "\"p90\": %.10g, \"p95\": %.10g},\n",
            advanced->count, advanced->invalid, advanced->mean,
            advanced->variance, advanced->standard_deviation,
            advanced->coefficient_variation, advanced->skewness,
            advanced->excess_kurtosis, advanced->p90, advanced->p95);
    if (poisson) {
        fprintf(out, "  \"poisson\": {\"eventos\": %zu, \"exposicion\": %.10g, "
                     "\"factor\": %.10g, \"tasa\": %.10g, \"inferior\": %.10g, "
                     "\"superior\": %.10g, \"aproximado\": %s},\n",
                poisson->events, poisson->exposure, poisson->factor, poisson->rate,
                poisson->lower, poisson->upper, poisson->approximate ? "true" : "false");
    }
    if (risk) {
        fprintf(out, "  \"riesgo\": {\"riesgo_expuesto\": %.10g, "
                     "\"riesgo_control\": %.10g, \"riesgo_relativo\": %.10g, "
                     "\"rr_inferior\": %.10g, \"rr_superior\": %.10g, "
                     "\"odds_ratio\": %.10g, \"or_inferior\": %.10g, "
                     "\"or_superior\": %.10g, \"correccion_continuidad\": %s},\n",
                risk->risk_exposed, risk->risk_control, risk->relative_risk,
                risk->rr_lower, risk->rr_upper, risk->odds_ratio,
                risk->or_lower, risk->or_upper,
                risk->continuity_correction ? "true" : "false");
    }
    if (mann_whitney) {
        fprintf(out, "  \"mann_whitney\": {\"n_primero\": %zu, \"n_segundo\": %zu, "
                     "\"u\": %.10g, \"z\": %.10g, \"p\": %.10g, \"aproximado\": %s},\n",
                mann_whitney->n_first, mann_whitney->n_second, mann_whitney->u,
                mann_whitney->z, mann_whitney->p_value,
                mann_whitney->approximate ? "true" : "false");
    }
    if (wilcoxon) {
        fprintf(out, "  \"wilcoxon\": {\"pares\": %zu, \"estadistico\": %.10g, "
                     "\"z\": %.10g, \"p\": %.10g, \"aproximado\": %s},\n",
                wilcoxon->pairs, wilcoxon->statistic, wilcoxon->z,
                wilcoxon->p_value, wilcoxon->approximate ? "true" : "false");
    }
    if (chi_square) {
        fprintf(out, "  \"chi_cuadrado\": {\"estadistico\": %.10g, "
                     "\"grados_libertad\": %zu, \"celdas_esperadas_bajas\": %zu, "
                     "\"valido\": %s},\n",
                chi_square->statistic, chi_square->degrees_of_freedom,
                chi_square->low_expected_cells, chi_square->valid ? "true" : "false");
    } else {
        fputs("  \"advertencia\": \"Los resultados inferenciales son opcionales y deben interpretarse con sus supuestos\",\n", out);
    }
    fputs("  \"advertencias\": [\n", out);
    fputs("    {\"tipo\": \"causalidad\", \"mensaje\": ", out);
    milena_json_write_string(out, "Asociación estadística no implica causalidad; pueden existir confusores, sesgo de selección o azar.");
    fputs("},\n    {\"tipo\": \"metodo\", \"mensaje\": ", out);
    milena_json_write_string(out, "Los resultados aproximados deben interpretarse con sus supuestos y tamaño muestral.");
    fputs("}\n  ]\n}\n", out);
    bool io_error = ferror(out) != 0;
    if (fclose(out) != 0) io_error = true;
    if (io_error) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "Error escribiendo reporte avanzado SST");
        return MILENA_ERR_IO;
    }
    return MILENA_OK;
}
