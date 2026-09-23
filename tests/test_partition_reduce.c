#include "partition_reduce.h"

#include <assert.h>
#include <math.h>

int main(void) {
    MilenaPhysicalPlan plan;
    milena_physical_plan_init(&plan);
    MilenaError error;
    milena_error_clear(&error);
    assert(milena_physical_plan_build(4096, 1024, 8, &plan, &error) == MILENA_OK);

    MilenaPartitionPartial partials[] = {
        {0, 0.1, true}, {1, 0.2, true}, {2, 0.3, true}, {3, 0.4, true}
    };
    double result = 0.0;
    assert(milena_partition_reduce_sum(&plan, partials, 4, &result,
                                       &error) == MILENA_OK);
    assert(fabs(result - 1.0) < 1e-12);

    partials[2].partition_id = 3;
    assert(milena_partition_reduce_sum(&plan, partials, 4, &result,
                                       &error) == MILENA_ERR_DATA);
    partials[2].partition_id = 2;
    partials[1].valid = false;
    assert(milena_partition_reduce_sum(&plan, partials, 4, &result,
                                       &error) == MILENA_OK);
    assert(fabs(result - 0.8) < 1e-12);
    partials[1].valid = true;
    partials[1].value = NAN;
    assert(milena_partition_reduce_sum(&plan, partials, 4, &result,
                                       &error) == MILENA_ERR_DATA);
    milena_physical_plan_release(&plan);
    puts("partition reduction tests passed");
    return 0;
}
