#ifndef MILENA_LANGUAGE_GROUPED_SPILL_H
#define MILENA_LANGUAGE_GROUPED_SPILL_H

#include "ast.h"
#include "table.h"

/* Typed adapter used only by the canonical .agrupar runtime branch. */
MilenaStatus milena_language_group_by_spill(
    MilenaTable *out, const MilenaTable *source, const char *key_column,
    const MilenaAggregateSpec *specification, const ASTNode *policy,
    MilenaError *error);

#endif
