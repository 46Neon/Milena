#include "common.h"

static bool token_equals(const char *actual, const char *expected) {
    if (!expected) return actual == NULL;
    return actual && strcmp(actual, expected) == 0;
}

static void check_token(const char *name, char *actual, const char *expected,
                        int *failures) {
    if (token_equals(actual, expected)) return;
    fprintf(stderr, "FAIL %s: expected %s, got %s\n", name,
            expected ? expected : "<null>", actual ? actual : "<null>");
    (*failures)++;
}

int main(void) {
    int failures = 0;

    char fields[] = ",alpha,,beta,\r\n";
    char *field_state = NULL;
    check_token("skip delimiter runs, first",
                milena_token_next(fields, ",\r\n", &field_state),
                "alpha", &failures);
    if (fields[6] != '\0') {
        fprintf(stderr, "FAIL tokenizer must replace delimiter with NUL\n");
        failures++;
    }
    check_token("skip delimiter runs, second",
                milena_token_next(NULL, ",\r\n", &field_state),
                "beta", &failures);
    if (fields[12] != '\0') {
        fprintf(stderr, "FAIL tokenizer must terminate each token in place\n");
        failures++;
    }
    check_token("finish delimiter runs",
                milena_token_next(NULL, ",\r\n", &field_state),
                NULL, &failures);

    char left[] = "x,y";
    char right[] = "1,2";
    char *left_state = NULL;
    char *right_state = NULL;
    check_token("independent state, left first",
                milena_token_next(left, ",", &left_state), "x", &failures);
    check_token("independent state, right first",
                milena_token_next(right, ",", &right_state), "1", &failures);
    check_token("independent state, left second",
                milena_token_next(NULL, ",", &left_state), "y", &failures);
    check_token("independent state, right second",
                milena_token_next(NULL, ",", &right_state), "2", &failures);
    check_token("independent state, left end",
                milena_token_next(NULL, ",", &left_state), NULL, &failures);
    check_token("independent state, right end",
                milena_token_next(NULL, ",", &right_state), NULL, &failures);

    char delimiters_only[] = ",,,";
    char *empty_state = NULL;
    check_token("delimiter-only input",
                milena_token_next(delimiters_only, ",", &empty_state),
                NULL, &failures);
    if (milena_token_next(NULL, ",", NULL) != NULL) {
        fprintf(stderr, "FAIL null state must return null\n");
        failures++;
    }

    if (failures != 0) return 1;
    puts("common tokenizer tests passed");
    return 0;
}
