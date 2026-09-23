#include "external_sort.h"

#include <assert.h>

typedef struct { const double *values; size_t count; size_t index; } Source;
static MilenaStatus next_value(void *context, double *value, bool *has,
                               MilenaError *error) {
    (void)error; Source *source = (Source *)context;
    if (source->index == source->count) { *has = false; return MILENA_OK; }
    *value = source->values[source->index++]; *has = true; return MILENA_OK;
}
static double decode(const unsigned char in[8]) {
    uint64_t bits=0; for(size_t i=0;i<8;i++) bits|=((uint64_t)in[i])<<(8u*i);
    double value=0.0; memcpy(&value,&bits,sizeof(value)); return value;
}
typedef struct { const double *expected; size_t count; size_t index; } Check;
static MilenaStatus verify(const void *data,size_t length,size_t index,
                           void *context,MilenaError *error) {
    (void)error; Check *check=(Check *)context;
    assert(length==8 && index==check->index && index<check->count);
    assert(decode((const unsigned char *)data)==check->expected[index]);
    check->index++; return MILENA_OK;
}
int main(void) {
    const double values[]={9,1,8,2,7,3,6,4,5};
    const double expected[]={1,2,3,4,5,6,7,8,9};
    Source source={values,9,0}; Check check={expected,9,0};
    const char *output="milena-external-sort.spill";
    (void)remove(output); (void)remove("milena-sort-runs.p0.r0.spill");
    MilenaError error; milena_error_clear(&error);
    assert(milena_external_sort_doubles(next_value,&source,"milena-sort-runs",
             output,2,2,4096,20000,&error)==MILENA_OK);
    assert(milena_spill_store_visit(output,4096,8,verify,&check,&error)==MILENA_OK);
    assert(check.index==9);
    assert(fopen("milena-sort-runs.p0.r0.spill","rb")==NULL);

    Source constrained={values,9,0};
    assert(milena_external_sort_doubles(next_value,&constrained,"milena-tight-runs",
             "milena-tight-output.spill",2,2,4096,100,&error)==MILENA_ERR_OVERFLOW);
    assert(fopen("milena-tight-output.spill","rb")==NULL);
    (void)remove(output);
    puts("external sort tests passed");
    return 0;
}
