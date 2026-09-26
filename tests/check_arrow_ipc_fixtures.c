#include "../tools/milena_sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    uint64_t expected_size;
    const char *expected_sha256;
} Fixture;

static const Fixture fixtures[] = {
    {"generated_primitive_cpp21.stream", 7152U,
     "124a70f6607c24dbdb50080ba953bb0f3156e3f8f4d0ea48f58b36f86c6b6e5d"},
    {"generated_binary_cpp21.stream", 13392U,
     "d284f820575fdc28adaba6808a2499446ed50672122f6a47742065b0541f24d4"},
    {"int64_above_2_53_pyarrow23.stream", 480U,
     "595811b8fc65f05b2972befa9a9dbde7a96ce08102b994993e80d469d50115f5"}
};

int main(int argc, char **argv)
{
    size_t index;
    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s FIXTURE_DIRECTORY\n", argv[0]);
        return 2;
    }
    for (index = 0U; index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
        char path[4096];
        char actual_sha256[65];
        uint64_t actual_size = 0U;
        int written = snprintf(path, sizeof(path), "%s/%s", argv[1], fixtures[index].name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            (void)fprintf(stderr, "Fixture path is too long: %s\n", fixtures[index].name);
            return 1;
        }
        if (milena_sha256_file(path, actual_sha256, &actual_size) != 0) {
            (void)fprintf(stderr, "Fixture unreadable: %s\n", path);
            return 1;
        }
        if (actual_size != fixtures[index].expected_size ||
            strcmp(actual_sha256, fixtures[index].expected_sha256) != 0) {
            (void)fprintf(stderr, "Fixture mismatch: %s: %" PRIu64 " bytes, %s\n",
                          fixtures[index].name, actual_size, actual_sha256);
            return 1;
        }
    }
    (void)printf("OK: %zu Arrow IPC fixture hashes\n",
                 sizeof(fixtures) / sizeof(fixtures[0]));
    return 0;
}
