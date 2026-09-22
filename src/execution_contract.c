#include "execution_contract.h"
#include <string.h>


MilenaOperatorCapabilities milena_operator_capabilities(MilenaLogicalOperatorKind k) {
    MilenaOperatorCapabilities c = {0,0,0,0,0,0};
    switch (k) {
    case MILENA_OPERATOR_AGGREGATE: c.streamable=1; c.associative=1; c.parallelizable=1; break;
    case MILENA_OPERATOR_FILTER: c.streamable=1; c.parallelizable=1; break;
    case MILENA_OPERATOR_PROJECT: c.streamable=1; c.parallelizable=1; break;
    case MILENA_OPERATOR_SORT: c.requires_materialization=1; c.order_sensitive=1; c.parallelizable=1; c.requires_shuffle=1; break;
    case MILENA_OPERATOR_JOIN: c.requires_materialization=1; c.parallelizable=1; c.requires_shuffle=1; break;
    case MILENA_OPERATOR_WINDOW: c.requires_materialization=1; c.order_sensitive=1; c.requires_shuffle=1; break;
    default: break;
    } return c;
}
const char *milena_operator_kind_name(MilenaLogicalOperatorKind k) {
    switch(k) { case MILENA_OPERATOR_AGGREGATE:return "agregacion"; case MILENA_OPERATOR_FILTER:return "filtro"; case MILENA_OPERATOR_PROJECT:return "proyeccion"; case MILENA_OPERATOR_SORT:return "ordenamiento"; case MILENA_OPERATOR_JOIN:return "union"; case MILENA_OPERATOR_WINDOW:return "ventana"; default:return "desconocido"; }
}

MilenaExecutionPlan milena_execution_plan_default(void) {
    MilenaExecutionPlan p;
    memset(&p, 0, sizeof(p));
    p.source = MILENA_EXEC_SOURCE_TABLE;
    p.options = milena_stream_options_default();
    p.sink = MILENA_EXEC_SINK_TABLE;
    p.operator_kind = MILENA_OPERATOR_AGGREGATE;
    p.capabilities = milena_operator_capabilities(p.operator_kind);
    return p;
}

MilenaStatus milena_execution_validate(const MilenaExecutionPlan *p, MilenaError *e) {
    if (!p || p->operator_kind > MILENA_OPERATOR_WINDOW || !p->aggregates || p->aggregate_count == 0 || p->aggregate_count > 64) {
        milena_error_set(e, MILENA_ERR_ARGUMENT, 0, 0, 0, "Plan de ejecución sin agregaciones válidas");
        return MILENA_ERR_ARGUMENT;
    }
    if (p->operator_kind != MILENA_OPERATOR_AGGREGATE) {
        milena_error_set(e, MILENA_ERR_UNSUPPORTED, 0, 0, 0, "Operador lógico aún no tiene backend canónico");
        return MILENA_ERR_UNSUPPORTED;
    }
    if (p->projection_column || p->filter_expression) {
        milena_error_set(e, MILENA_ERR_UNSUPPORTED, 0, 0, 0, "Proyección/filtro aún no tienen backend común");
        return MILENA_ERR_UNSUPPORTED;
    }
    for (size_t i = 0; i < p->aggregate_count; ++i) {
        if (!p->aggregates[i].column || p->aggregates[i].column[0] == '\0' ||
            p->aggregates[i].operation < MILENA_AGG_COUNT || p->aggregates[i].operation > MILENA_AGG_MAX) {
            milena_error_set(e, MILENA_ERR_ARGUMENT, 0, 0, 0, "Agregación fuera del contrato canónico");
            return MILENA_ERR_ARGUMENT;
        }
    }
    return MILENA_OK;
}

MilenaStatus milena_stream_execute_plan(const MilenaExecutionPlan *p, const char *in, const char *out, MilenaExecutionReport *r, MilenaError *e) {
    MilenaStatus st = milena_execution_validate(p, e);
    if (st != MILENA_OK || !in || !out || p->source != MILENA_EXEC_SOURCE_CSV_STREAM || p->sink != MILENA_EXEC_SINK_JSON_REPORT) {
        if (st == MILENA_OK) { milena_error_set(e, MILENA_ERR_ARGUMENT, 0, 0, 0, "Fuente/salida incompatibles con backend de flujo"); st = MILENA_ERR_ARGUMENT; }
        if (r) { memset(r, 0, sizeof(*r)); r->status = st; r->backend = "stream"; }
        return st;
    }
    MilenaStreamMetric metrics[64];
    for (size_t i = 0; i < p->aggregate_count; ++i) {
        metrics[i].column = p->aggregates[i].column;
        metrics[i].name = p->aggregates[i].output_name;
        switch (p->aggregates[i].operation) {
            case MILENA_AGG_COUNT: metrics[i].operation = MILENA_STREAM_COUNT; break;
            case MILENA_AGG_SUM: metrics[i].operation = MILENA_STREAM_SUM; break;
            case MILENA_AGG_MEAN: metrics[i].operation = MILENA_STREAM_MEAN; break;
            case MILENA_AGG_MIN: metrics[i].operation = MILENA_STREAM_MIN; break;
            case MILENA_AGG_MAX: metrics[i].operation = MILENA_STREAM_MAX; break;
            default: st = MILENA_ERR_UNSUPPORTED; milena_error_set(e, st, 0, 0, 0, "Operación no soportada por flujo"); break;
        }
        if (st != MILENA_OK) break;
    }
    MilenaStreamReport sr;
    if (p->group_column) st = milena_stream_csv_grouped_with_options(in, out, p->group_column, metrics, p->aggregate_count, &p->options, &sr, e);
    else st = milena_stream_csv_summary_with_options(in, out, metrics, p->aggregate_count, &p->options, &sr, e);
    if (r) { memset(r, 0, sizeof(*r)); r->status = st; r->backend = "stream"; r->rows_read = sr.rows_read; r->malformed_rows = sr.malformed_rows; r->peak_record_bytes = sr.peak_record_bytes; r->input_bytes = sr.input_bytes; r->elapsed_milliseconds = sr.elapsed_milliseconds; }
    return st;
}

MilenaStatus milena_table_execute_plan(MilenaTable *out, const MilenaTable *source, const MilenaExecutionPlan *p, MilenaExecutionReport *r, MilenaError *e) {
    MilenaStatus st = milena_execution_validate(p, e);
    if (st != MILENA_OK || !out || !source || p->source != MILENA_EXEC_SOURCE_TABLE || p->sink != MILENA_EXEC_SINK_TABLE) {
        if (st == MILENA_OK) { milena_error_set(e, MILENA_ERR_ARGUMENT, 0, 0, 0, "Fuente/salida incompatibles con backend de tabla"); st = MILENA_ERR_ARGUMENT; }
        if (r) { memset(r, 0, sizeof(*r)); r->status = st; r->backend = "table"; }
        return st;
    }
    MilenaAggregateSpec specs[64];
    for (size_t i = 0; i < p->aggregate_count; ++i) { specs[i].value_column = p->aggregates[i].column; specs[i].operation = p->aggregates[i].operation; specs[i].output_name = p->aggregates[i].output_name; }
    if (p->group_column) st = milena_table_group_by(out, source, &p->group_column, 1, specs, p->aggregate_count, e);
    else st = milena_table_summarize(out, source, specs, p->aggregate_count, e);
    if (r) { memset(r, 0, sizeof(*r)); r->status = st; r->backend = "table"; r->rows_read = source->row_count; r->rows_emitted = out->row_count; r->groups = p->group_column ? out->row_count : 1; }
    return st;
}
