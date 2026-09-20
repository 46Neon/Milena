#include "table.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void ok(MilenaStatus status, MilenaError *error) {
    if (status != MILENA_OK) {
        fprintf(stderr, "FAIL %s: %s\n", milena_status_name(status),
                error->message);
        assert(status == MILENA_OK);
    }
    milena_error_clear(error);
}

static MilenaArray make_array(MilenaDType dtype, size_t count,
                              const void *data, MilenaError *error) {
    size_t shape[] = {count};
    MilenaArray result = {0};
    ok(milena_array_zeros(&result, dtype, 1, shape, error), error);
    if (count != 0) memcpy(milena_array_data(&result), data,
                           count * result.itemsize);
    return result;
}

static void test_schema_strings_categorical(MilenaError *error) {
    const int32_t integers[] = {3, 1, 2, 4};
    const bool bools[] = {true, false, true, false};
    const uint8_t codes[] = {0, 1, 0, 1};
    const char *strings[] = {"árbol", "beta", NULL, "delta"};
    const char *dictionary[] = {"rojo", "azul"};
    const bool string_valid[] = {true, true, false, true};
    MilenaArray ia = make_array(MILENA_DTYPE_INT32, 4, integers, error);
    MilenaArray ba = make_array(MILENA_DTYPE_BOOL, 4, bools, error);
    MilenaArray ca = make_array(MILENA_DTYPE_UINT8, 4, codes, error);
    MilenaTable table = {0};
    milena_table_init(&table);
    ok(milena_table_add_column_copy(&table, "i", &ia, NULL, error), error);
    ok(milena_table_add_column_copy(&table, "b", &ba, NULL, error), error);
    ok(milena_table_add_string_column_copy(&table, "s", strings, 4,
                                            string_valid, error), error);
    ok(milena_table_add_categorical_column_copy(&table, "c", &ca,
                                                 dictionary, 2, NULL,
                                                 error), error);
    ok(milena_table_validate(&table, error), error);
    assert(milena_table_add_column_copy(&table, "i", &ia, NULL, error) ==
           MILENA_ERR_ARGUMENT);
    milena_error_clear(error);
    const char *text = NULL;
    ok(milena_table_get_string(&table, 2, 0, &text, error), error);
    assert(strcmp(text, "árbol") == 0);
    ok(milena_table_get_string(&table, 2, 2, &text, error), error);
    assert(text == NULL && milena_table_is_null(&table, 2, 2));
    uint32_t code = 9; const char *label = NULL;
    ok(milena_table_get_category(&table, 3, 1, &code, &label, error), error);
    assert(code == 1 && strcmp(label, "azul") == 0);
    ok(milena_table_set_metadata(&table, "owner", "worker4", error), error);
    ok(milena_table_column_set_metadata(&table, "s", "logical", "utf8",
                                         error), error);
    assert(strcmp(milena_table_get_metadata(&table, "owner"), "worker4") == 0);
    assert(strcmp(milena_table_column_get_metadata(&table, "s", "logical"),
                  "utf8") == 0);

    MilenaTable clone = {0};
    milena_table_init(&clone);
    ok(milena_table_clone(&clone, &table, error), error);
    table.columns[2].strings[1][0] = 'B';
    ok(milena_table_get_string(&clone, 2, 1, &text, error), error);
    assert(strcmp(text, "beta") == 0);

    MilenaTableValue fill = {0};
    fill.type = MILENA_COLUMN_STRING;
    fill.as.string = "relleno";
    ok(milena_table_fill_null(&table, "s", &fill, error), error);
    ok(milena_table_get_string(&table, 2, 2, &text, error), error);
    assert(strcmp(text, "relleno") == 0);

    milena_table_destroy(&clone);
    milena_table_destroy(&table);
    milena_array_release(&ca); milena_array_release(&ba); milena_array_release(&ia);
}

static void test_filter_select_drop_failure(MilenaError *error) {
    const int64_t values[] = {10, 20, 30, 40, 50, 60};
    const bool validity[] = {true, false, true, true, true, true};
    const bool masks[] = {true, false, false, true, true, false};
    MilenaArray va = make_array(MILENA_DTYPE_INT64, 6, values, error);
    MilenaArray ma = make_array(MILENA_DTYPE_BOOL, 6, masks, error);
    MilenaSlice slice = {true, 0, true, 6, 2};
    MilenaArray strided = {0};
    ok(milena_array_slice_view_ex(&strided, &ma, 0, &slice, error), error);
    MilenaSlice value_slice = {true, 0, true, 6, 2};
    MilenaArray value_view = {0};
    ok(milena_array_slice_view_ex(&value_view, &va, 0, &value_slice, error), error);
    const bool selected_validity[] = {true, true, true};
    MilenaTable source = {0};
    milena_table_init(&source);
    ok(milena_table_add_column_copy(&source, "v", &value_view,
                                     selected_validity, error), error);
    MilenaTable filtered = {0}; milena_table_init(&filtered);
    ok(milena_table_filter(&filtered, &source, &strided, error), error);
    assert(filtered.row_count == 2);
    const int64_t *fv = (const int64_t *)milena_array_const_data(
        &filtered.columns[0].values);
    assert(fv[0] == 10 && fv[1] == 50);

    const char *names[] = {"v"};
    MilenaTable selected = {0}; milena_table_init(&selected);
    ok(milena_table_select_columns(&selected, &source, names, 1, error), error);
    MilenaTable before = {0}; milena_table_init(&before);
    ok(milena_table_clone(&before, &selected, error), error);
    assert(milena_table_sort(&selected, &source, "missing", true, error) ==
           MILENA_ERR_DATA);
    assert(selected.row_count == before.row_count &&
           selected.column_count == before.column_count);
    milena_error_clear(error);

    MilenaTable nullable = {0}; milena_table_init(&nullable);
    ok(milena_table_add_column_copy(&nullable, "v", &va, validity, error), error);
    MilenaTable dropped = {0}; milena_table_init(&dropped);
    ok(milena_table_drop_null(&dropped, &nullable, error), error);
    assert(dropped.row_count == 5);
    MilenaTableValue fill = {0}; fill.type = MILENA_COLUMN_ARRAY;
    fill.dtype = MILENA_DTYPE_INT64; fill.as.i64 = -7;
    ok(milena_table_fill_null(&nullable, "v", &fill, error), error);
    const int64_t *filled = (const int64_t *)milena_array_const_data(
        &nullable.columns[0].values);
    assert(filled[1] == -7 && nullable.columns[0].validity[1]);

    milena_table_destroy(&dropped); milena_table_destroy(&nullable);
    milena_table_destroy(&before); milena_table_destroy(&selected);
    milena_table_destroy(&filtered); milena_table_destroy(&source);
    milena_array_release(&value_view); milena_array_release(&strided);
    milena_array_release(&ma); milena_array_release(&va);
}

static void test_sort(MilenaError *error) {
    const int64_t ids[] = {0, 1, 2, 3, 4, 5};
    const double values[] = {2.0, NAN, 1.0, 2.0, 0.0, 3.0};
    const bool valid[] = {true, true, true, true, false, true};
    const char *names[] = {"b", "a", "a", "b", "a", "a"};
    MilenaArray id = make_array(MILENA_DTYPE_INT64, 6, ids, error);
    MilenaArray value = make_array(MILENA_DTYPE_FLOAT64, 6, values, error);
    MilenaTable source = {0}; milena_table_init(&source);
    ok(milena_table_add_column_copy(&source, "id", &id, NULL, error), error);
    ok(milena_table_add_column_copy(&source, "value", &value, valid, error), error);
    ok(milena_table_add_string_column_copy(&source, "name", names, 6, NULL,
                                            error), error);
    MilenaTable sorted = {0}; milena_table_init(&sorted);
    ok(milena_table_sort(&sorted, &source, "value", true, error), error);
    const int64_t *order = (const int64_t *)milena_array_const_data(
        &sorted.columns[0].values);
    assert(order[0] == 2 && order[1] == 0 && order[2] == 3 && order[3] == 5 &&
           order[4] == 1 && order[5] == 4);
    MilenaSortKey keys[] = {{"name", true}, {"value", false}};
    ok(milena_table_sort_keys(&sorted, &source, keys, 2, error), error);
    order = (const int64_t *)milena_array_const_data(&sorted.columns[0].values);
    assert(order[0] == 5 && order[1] == 2 && order[2] == 1 && order[3] == 4);
    milena_table_destroy(&sorted); milena_table_destroy(&source);
    milena_array_release(&value); milena_array_release(&id);
}

static void test_group_and_scale(MilenaError *error) {
    const int64_t key[] = {1, 1, 2, 2, 1};
    const int64_t sub[] = {0, 0, 0, 1, 0};
    const int32_t value[] = {10, 20, 3, 4, 5};
    const bool valid[] = {true, true, false, true, true};
    MilenaArray ka = make_array(MILENA_DTYPE_INT64, 5, key, error);
    MilenaArray sa = make_array(MILENA_DTYPE_INT64, 5, sub, error);
    MilenaArray va = make_array(MILENA_DTYPE_INT32, 5, value, error);
    MilenaTable source = {0}; milena_table_init(&source);
    ok(milena_table_add_column_copy(&source, "k", &ka, NULL, error), error);
    ok(milena_table_add_column_copy(&source, "sub", &sa, NULL, error), error);
    ok(milena_table_add_column_copy(&source, "v", &va, valid, error), error);
    const char *keys[] = {"k", "sub"};
    MilenaAggregateSpec specs[] = {{"v", MILENA_AGG_COUNT, "n"},
                                    {"v", MILENA_AGG_SUM, "total"},
                                    {"v", MILENA_AGG_MEAN, "avg"},
                                    {"v", MILENA_AGG_MIN, "low"},
                                    {"v", MILENA_AGG_MAX, "high"}};
    MilenaTable grouped = {0}; milena_table_init(&grouped);
    ok(milena_table_group_by(&grouped, &source, keys, 2, specs, 5, error), error);
    assert(grouped.row_count == 3);
    const int32_t *sum = (const int32_t *)milena_array_const_data(
        &grouped.columns[3].values);
    assert(sum[0] == 35 && sum[1] == 0 && sum[2] == 4);
    assert(!grouped.columns[3].validity[1]);

    const int8_t over[] = {INT8_MAX, 1};
    const int64_t same[] = {1, 1};
    MilenaArray oa = make_array(MILENA_DTYPE_INT8, 2, over, error);
    MilenaArray ga = make_array(MILENA_DTYPE_INT64, 2, same, error);
    MilenaTable overflow = {0}; milena_table_init(&overflow);
    ok(milena_table_add_column_copy(&overflow, "g", &ga, NULL, error), error);
    ok(milena_table_add_column_copy(&overflow, "x", &oa, NULL, error), error);
    MilenaTable preserved = {0}; milena_table_init(&preserved);
    ok(milena_table_clone(&preserved, &grouped, error), error);
    assert(milena_table_group_by_aggregate(&preserved, &overflow, "g", "x",
                                            MILENA_AGG_SUM, error) ==
           MILENA_ERR_OVERFLOW);
    assert(preserved.row_count == grouped.row_count);
    milena_error_clear(error);

    const size_t n = 100000;
    size_t shape[] = {n};
    MilenaArray large_keys = {0}; MilenaArray large_values = {0};
    ok(milena_array_zeros(&large_keys, MILENA_DTYPE_INT64, 1, shape, error), error);
    ok(milena_array_zeros(&large_values, MILENA_DTYPE_INT64, 1, shape, error), error);
    int64_t *lk = (int64_t *)milena_array_data(&large_keys);
    int64_t *lv = (int64_t *)milena_array_data(&large_values);
    for (size_t i = 0; i < n; ++i) { lk[i] = (int64_t)(i % 1000); lv[i] = 1; }
    MilenaTable large = {0}; milena_table_init(&large);
    ok(milena_table_add_column_copy(&large, "k", &large_keys, NULL, error), error);
    ok(milena_table_add_column_copy(&large, "v", &large_values, NULL, error), error);
    MilenaTable large_group = {0}; milena_table_init(&large_group);
    ok(milena_table_group_by_aggregate(&large_group, &large, "k", "v",
                                        MILENA_AGG_SUM, error), error);
    assert(large_group.row_count == 1000);
    assert(((const int64_t *)milena_array_const_data(
               &large_group.columns[1].values))[0] == 100);

    milena_table_destroy(&large_group); milena_table_destroy(&large);
    milena_array_release(&large_values); milena_array_release(&large_keys);
    milena_table_destroy(&preserved); milena_table_destroy(&overflow);
    milena_array_release(&ga); milena_array_release(&oa);
    milena_table_destroy(&grouped); milena_table_destroy(&source);
    milena_array_release(&va); milena_array_release(&sa); milena_array_release(&ka);
}

static MilenaTable join_table(const int64_t *keys, const char *const *values,
                              const bool *valid, size_t count,
                              const char *value_name, MilenaError *error) {
    MilenaArray ka = make_array(MILENA_DTYPE_INT64, count, keys, error);
    MilenaTable result = {0}; milena_table_init(&result);
    ok(milena_table_add_column_copy(&result, "key", &ka, valid, error), error);
    ok(milena_table_add_string_column_copy(&result, value_name, values, count,
                                            NULL, error), error);
    milena_array_release(&ka);
    return result;
}

static void test_joins_unpivot(MilenaError *error) {
    const int64_t lk[] = {1, 1, 2, 9};
    const int64_t rk[] = {1, 1, 3, 9};
    const bool lv[] = {true, true, true, false};
    const bool rv[] = {true, true, true, false};
    const char *la[] = {"l0", "l1", "l2", "ln"};
    const char *ra[] = {"r0", "r1", "r3", "rn"};
    MilenaTable left = join_table(lk, la, lv, 4, "value", error);
    MilenaTable right = join_table(rk, ra, rv, 4, "value", error);
    const char *keys[] = {"key"};
    MilenaTable result = {0}; milena_table_init(&result);
    ok(milena_table_join(&result, &left, &right, keys, keys, 1,
                          MILENA_JOIN_INNER, error), error);
    assert(result.row_count == 4 &&
           milena_table_column_index(&result, "key_right") >= 0 &&
           milena_table_column_index(&result, "value_right") >= 0);
    ok(milena_table_join(&result, &left, &right, keys, keys, 1,
                          MILENA_JOIN_LEFT, error), error);
    assert(result.row_count == 6);
    ok(milena_table_join(&result, &left, &right, keys, keys, 1,
                          MILENA_JOIN_RIGHT, error), error);
    assert(result.row_count == 6);
    ok(milena_table_join(&result, &left, &right, keys, keys, 1,
                          MILENA_JOIN_FULL, error), error);
    assert(result.row_count == 8);

    const int64_t ids[] = {10, 11};
    const double a[] = {1.0, 2.0};
    const double b[] = {3.0, 4.0};
    MilenaArray ia = make_array(MILENA_DTYPE_INT64, 2, ids, error);
    MilenaArray aa = make_array(MILENA_DTYPE_FLOAT64, 2, a, error);
    MilenaArray ba = make_array(MILENA_DTYPE_FLOAT64, 2, b, error);
    MilenaTable wide = {0}; milena_table_init(&wide);
    ok(milena_table_add_column_copy(&wide, "id", &ia, NULL, error), error);
    ok(milena_table_add_column_copy(&wide, "a", &aa, NULL, error), error);
    ok(milena_table_add_column_copy(&wide, "b", &ba, NULL, error), error);
    const char *id_names[] = {"id"}; const char *value_names[] = {"a", "b"};
    MilenaTable long_table = {0}; milena_table_init(&long_table);
    ok(milena_table_unpivot(&long_table, &wide, id_names, 1, value_names, 2,
                             "variable", "value", error), error);
    assert(long_table.row_count == 4);
    const int64_t *long_ids = (const int64_t *)milena_array_const_data(
        &long_table.columns[0].values);
    const double *long_values = (const double *)milena_array_const_data(
        &long_table.columns[2].values);
    assert(long_ids[0] == 10 && long_ids[1] == 10 && long_ids[2] == 11);
    assert(long_values[0] == 1.0 && long_values[1] == 3.0 &&
           long_values[2] == 2.0 && long_values[3] == 4.0);

    milena_table_destroy(&long_table); milena_table_destroy(&wide);
    milena_array_release(&ba); milena_array_release(&aa); milena_array_release(&ia);
    milena_table_destroy(&result); milena_table_destroy(&right);
    milena_table_destroy(&left);
}

static void test_empty_and_legacy(MilenaError *error) {
    size_t shape[] = {0};
    MilenaArray empty = {0};
    ok(milena_array_zeros(&empty, MILENA_DTYPE_FLOAT64, 1, shape, error), error);
    MilenaTable table = {0}; milena_table_init(&table);
    ok(milena_table_add_column_copy(&table, "x", &empty, NULL, error), error);
    MilenaTable out = {0}; milena_table_init(&out);
    ok(milena_table_sort(&out, &table, "x", false, error), error);
    assert(out.row_count == 0 && out.column_count == 1);
    ok(milena_table_fill_null_f64(&table, "x", 0.0, error), error);
    milena_table_destroy(&out); milena_table_destroy(&table);
    milena_array_release(&empty);
}

int main(void) {
    MilenaError error; milena_error_clear(&error);
    test_schema_strings_categorical(&error);
    test_filter_select_drop_failure(&error);
    test_sort(&error);
    test_group_and_scale(&error);
    test_joins_unpivot(&error);
    test_empty_and_legacy(&error);
    puts("OK: worker4 typed tables, relational operations and 100k scale");
    return 0;
}
