#include "partition_protocol.h"

#include <assert.h>
#include <string.h>

int main(void) {
    unsigned char buffer[MILENA_PARTITION_RESULT_WIRE_SIZE];
    size_t written = 0;
    MilenaError error;
    milena_error_clear(&error);
    MilenaPartitionResult original = {17, MILENA_OK, true, 123.5};
    MilenaPartitionResult decoded = {0};
    assert(milena_partition_result_encode(&original, buffer, sizeof(buffer),
                                          &written, &error) == MILENA_OK);
    assert(written == sizeof(buffer));
    /* Version and checksum are encoded byte-by-byte, independent of host endianness. */
    assert(buffer[4] == 2u && buffer[5] == 0u && buffer[6] == 0u && buffer[7] == 0u);
    assert(buffer[8] == 0u && buffer[9] == 0u && buffer[10] == 0u && buffer[11] == 0u);
    assert(milena_partition_result_decode(buffer, written, &decoded,
                                          &error) == MILENA_OK);
    assert(decoded.partition_id == original.partition_id);
    assert(decoded.status == original.status);
    assert(decoded.valid == original.valid);
    assert(decoded.value == original.value);

    buffer[12] ^= 1u;
    assert(milena_partition_result_decode(buffer, written, &decoded,
                                          &error) == MILENA_ERR_DATA);
    buffer[12] ^= 1u;
    buffer[4] = 2u;
    assert(milena_partition_result_decode(buffer, written, &decoded,
                                          &error) == MILENA_ERR_PARSE);
    assert(milena_partition_result_decode(buffer, written - 1, &decoded,
                                          &error) == MILENA_ERR_ARGUMENT);
    puts("partition protocol tests passed");
    return 0;
}
