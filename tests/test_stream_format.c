#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Include the implementation in this isolated unit to exercise private run-format
 * readers/writers without exposing test hooks in the product API. */
#include "../src/stream.c"

int main(void) {
    const char *path = "tests/.stream_format.run";
    FILE *file = fopen(path, "w+b");
    assert(file != NULL);
    assert(spill_run_header_write(file, 1));
    StreamAccumulator accumulator = {0};
    accumulator.sum = 12.5;
    accumulator.mean = 12.5;
    accumulator.minimum = 12.5;
    accumulator.maximum = 12.5;
    accumulator.count = 1;
    size_t invalid = 0;
    StreamGroup group = {"K", &accumulator, &invalid};
    size_t bytes = STREAM_SPILL_HEADER_BYTES;
    assert(spill_record_write(file, &group, 1, &bytes));
    assert(fflush(file) == 0);

    rewind(file);
    assert(spill_run_header_read(file, 1));
    StreamSpillRun run = {0};
    run.file = file;
    MilenaError error;
    milena_error_clear(&error);
    assert(spill_cursor_next(&run, 1, &error) == MILENA_OK);
    assert(run.active && strcmp(run.current.key, "K") == 0);
    assert(run.current.accumulators[0].sum == 12.5);
    spill_cursor_clear(&run);

    /* Unsupported format versions fail even with a valid header checksum. */
    unsigned char header[STREAM_SPILL_HEADER_BYTES];
    rewind(file);
    assert(fread(header, 1, sizeof(header), file) == sizeof(header));
    header[4] = 2;
    spill_put_u32(header + 12, spill_hash32(header, 12));
    rewind(file);
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    assert(fflush(file) == 0);
    rewind(file);
    assert(!spill_run_header_read(file, 1));

    /* Header checksum corruption is rejected independently. */
    header[4] = 1;
    spill_put_u32(header + 12, spill_hash32(header, 12));
    header[12] ^= 0x01;
    rewind(file);
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    assert(fflush(file) == 0);
    rewind(file);
    assert(!spill_run_header_read(file, 1));

    /* Restore the valid header and flip the stored record checksum. */
    header[4] = 1;
    spill_put_u32(header + 12, spill_hash32(header, 12));
    rewind(file);
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    assert(fflush(file) == 0);
    long checksum_offset = (long)STREAM_SPILL_HEADER_BYTES + 4L + 69L;
    assert(fseek(file, checksum_offset, SEEK_SET) == 0);
    int byte = fgetc(file);
    assert(byte != EOF);
    assert(fseek(file, checksum_offset, SEEK_SET) == 0);
    assert(fputc(byte ^ 0x80, file) != EOF);
    assert(fflush(file) == 0);
    rewind(file);
    assert(spill_run_header_read(file, 1));
    milena_error_clear(&error);
    assert(spill_cursor_next(&run, 1, &error) == MILENA_ERR_DATA);

    assert(fclose(file) == 0);
    assert(remove(path) == 0);
    puts("stream spill format tests passed");
    return 0;
}
