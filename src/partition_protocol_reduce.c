#include "partition_protocol_reduce.h"

static void wire_reduce_error(MilenaError *error, MilenaStatus status,
                              const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

MilenaStatus milena_partition_results_reduce_sum(
    const MilenaPhysicalPlan *plan,
    const unsigned char *messages,
    size_t message_count,
    double *result,
    MilenaError *error) {
    if (!plan || !result || (message_count > 0 && !messages) ||
        message_count != plan->partition_count) {
        wire_reduce_error(error, MILENA_ERR_ARGUMENT,
                          "La reducción distribuible requiere una respuesta por partición");
        return MILENA_ERR_ARGUMENT;
    }
    if (plan->partition_count == 0) {
        *result = 0.0;
        if (error) milena_error_clear(error);
        return MILENA_OK;
    }
    MilenaPartitionPartial *partials = (MilenaPartitionPartial *)calloc(
        plan->partition_count, sizeof(*partials));
    bool *seen = (bool *)calloc(plan->partition_count, sizeof(*seen));
    if (!partials || !seen) {
        free(partials);
        free(seen);
        wire_reduce_error(error, MILENA_ERR_MEMORY,
                          "Memoria insuficiente para resultados distribuidos");
        return MILENA_ERR_MEMORY;
    }
    MilenaStatus status = MILENA_OK;
    for (size_t i = 0; i < message_count; i++) {
        MilenaPartitionResult decoded = {0};
        status = milena_partition_result_decode(
            messages + i * MILENA_PARTITION_RESULT_WIRE_SIZE,
            MILENA_PARTITION_RESULT_WIRE_SIZE, &decoded, error);
        if (status != MILENA_OK) break;
        if (decoded.partition_id >= plan->partition_count) {
            wire_reduce_error(error, MILENA_ERR_DATA,
                              "El resultado pertenece a una partición desconocida");
            status = MILENA_ERR_DATA;
            break;
        }
        if (seen[decoded.partition_id]) {
            wire_reduce_error(error, MILENA_ERR_DATA,
                              "Se recibió más de un resultado para una partición");
            status = MILENA_ERR_DATA;
            break;
        }
        if (decoded.status != MILENA_OK) {
            wire_reduce_error(error, decoded.status,
                              "Una partición distribuida terminó con error");
            status = decoded.status;
            break;
        }
        seen[decoded.partition_id] = true;
        partials[decoded.partition_id].partition_id = decoded.partition_id;
        partials[decoded.partition_id].valid = decoded.valid;
        partials[decoded.partition_id].value = decoded.value;
    }
    if (status == MILENA_OK) {
        for (size_t i = 0; i < plan->partition_count; i++) {
            if (!seen[i]) {
                wire_reduce_error(error, MILENA_ERR_DATA,
                                  "Falta el resultado de una partición");
                status = MILENA_ERR_DATA;
                break;
            }
        }
    }
    if (status == MILENA_OK) {
        status = milena_partition_reduce_sum(plan, partials,
                                             plan->partition_count,
                                             result, error);
    }
    free(partials);
    free(seen);
    return status;
}
