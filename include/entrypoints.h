#ifndef MILENA_ENTRYPOINTS_H
#define MILENA_ENTRYPOINTS_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compatibility adapters for the historical CLI.  Each adapter validates a
 * minimal canonical Milena program before invoking the legacy output backend;
 * the backend's public format is intentionally unchanged.
 */
MilenaStatus milena_cli_analyze(const char *input_csv, const char *output_json,
                                FILE *output, MilenaError *error);
MilenaStatus milena_cli_profile(const char *input_csv, const char *output_json,
                                FILE *output, MilenaError *error);
MilenaStatus milena_cli_inspect(const char *input_csv, FILE *output,
                                MilenaError *error);
MilenaStatus milena_cli_run_script(const char *script_filename,
                                   MilenaError *error);

#ifdef __cplusplus
}
#endif

#endif
