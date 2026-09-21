#ifndef MILENA_LANGUAGE_RUNTIME_H
#define MILENA_LANGUAGE_RUNTIME_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ejecuta el subconjunto canonico de arrays ya representado por el AST:
 * declaraciones 1-D y operaciones estadisticas. */
MilenaStatus milena_run_array_program(const char *source, FILE *output,
                                      MilenaError *error);

/* Ejecuta el subconjunto canónico de datasets mediante el mismo AST/runtime. */
MilenaStatus milena_run_dataset_program(const char *source,
                                        const char *script_filename,
                                        FILE *output,
                                        MilenaError *error);

#ifdef __cplusplus
}
#endif

#endif
