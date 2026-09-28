#ifndef MILENA_NATIVE_AOT_H
#define MILENA_NATIVE_AOT_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build a direct native executable from the currently supported verified IR
 * subset. The Linux x86-64 backend writes an ELF64 executable itself and does
 * not invoke a C compiler, assembler, linker, VM, or interpreter. Unsupported
 * targets or IR constructs fail closed. */
MilenaStatus milena_cli_build(const char *source_filename,
                              const char *output_filename,
                              MilenaError *error);

#ifdef __cplusplus
}
#endif

#endif
