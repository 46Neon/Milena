#ifndef MILENA_NATIVE_AOT_H
#define MILENA_NATIVE_AOT_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build a native executable from the verified canonical typed-IR subset.
 * MILENA_CC selects one executable path (not a command line); the default is
 * cc. Building requires a POSIX host with fork/exec, mkstemp, and atomic rename. */
MilenaStatus milena_cli_build(const char *source_filename,
                              const char *output_filename,
                              MilenaError *error);

#ifdef __cplusplus
}
#endif

#endif
