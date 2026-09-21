#include "partition_plan.h"

#include <assert.h>

int main(void) {
    MilenaPhysicalPlan plan;
    milena_physical_plan_init(&plan);
    MilenaError error;
    milena_error_clear(&error);

    assert(milena_physical_plan_build(1000, 256, 8, &plan, &error) == MILENA_OK);
    assert(plan.partition_count == 4);
    assert(plan.partitions[0].offset_bytes == 0);
    assert(plan.partitions[0].length_bytes == 256);
    assert(plan.partitions[3].offset_bytes == 768);
    assert(plan.partitions[3].length_bytes == 232);
    assert(milena_physical_plan_validate(&plan, &error) == MILENA_OK);
    milena_physical_plan_release(&plan);

    assert(milena_physical_plan_build(1000, 256, 2, &plan, &error) == MILENA_ERR_OVERFLOW);
    assert(plan.partitions == NULL);

    assert(milena_physical_plan_build(0, 256, 1, &plan, &error) == MILENA_OK);
    assert(plan.partition_count == 0);
    assert(milena_physical_plan_validate(&plan, &error) == MILENA_OK);
    milena_physical_plan_release(&plan);
    puts("partition plan tests passed");
    return 0;
}
