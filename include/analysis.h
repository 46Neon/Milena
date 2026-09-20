#ifndef MILENA_ANALYSIS_H
#define MILENA_ANALYSIS_H

#include "dataset.h"
#include "schema.h"
#include "table.h"

typedef struct {
    size_t rows_seen;
    size_t rows_used;
    size_t rows_rejected;
    double total;
    double average;
    double minimum;
    double maximum;
} SalesSummary;

MilenaStatus analysis_dataset_report(const Dataset *dataset,
                                  const MilenaSchema *schema,
                                  const char *output_json,
                                  MilenaError *error);

MilenaStatus analysis_table_report(const MilenaTable *table,
                                const MilenaSchema *schema,
                                const char *output_json,
                                MilenaError *error);

MilenaStatus analysis_sales(const Dataset *dataset,
                          const char *date_column,
                          const char *price_column,
                          const char *quantity_column,
                          const char *output_json,
                          SalesSummary *summary,
                          MilenaError *error);

bool analysis_ventas(const Dataset *dataset, const char *date_column,
                     const char *price_column, const char *quantity_column,
                     const char *output_json);

#endif
