#include "milena_sha256.h"

#include <stdio.h>
#include <string.h>

#define SHA256_BLOCK_SIZE 64U
#define SHA256_DIGEST_SIZE 32U
#define ROTR32(value, count) (((value) >> (count)) | ((value) << (32U - (count))))

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[SHA256_BLOCK_SIZE];
    size_t block_length;
} Sha256Context;

static const uint32_t round_constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void sha256_transform(Sha256Context *context, const uint8_t block[SHA256_BLOCK_SIZE])
{
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        size_t offset = index * 4U;
        words[index] = ((uint32_t)block[offset] << 24U) |
                       ((uint32_t)block[offset + 1U] << 16U) |
                       ((uint32_t)block[offset + 2U] << 8U) |
                       (uint32_t)block[offset + 3U];
    }
    for (index = 16U; index < 64U; ++index) {
        uint32_t x = words[index - 15U];
        uint32_t y = words[index - 2U];
        uint32_t sigma0 = ROTR32(x, 7U) ^ ROTR32(x, 18U) ^ (x >> 3U);
        uint32_t sigma1 = ROTR32(y, 17U) ^ ROTR32(y, 19U) ^ (y >> 10U);
        words[index] = words[index - 16U] + sigma0 + words[index - 7U] + sigma1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (index = 0U; index < 64U; ++index) {
        uint32_t sum1 = ROTR32(e, 6U) ^ ROTR32(e, 11U) ^ ROTR32(e, 25U);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + sum1 + choose + round_constants[index] + words[index];
        uint32_t sum0 = ROTR32(a, 2U) ^ ROTR32(a, 13U) ^ ROTR32(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void sha256_init(Sha256Context *context)
{
    static const uint32_t initial_state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    memcpy(context->state, initial_state, sizeof(initial_state));
    context->bit_count = 0U;
    context->block_length = 0U;
}

static void sha256_update(Sha256Context *context, const uint8_t *data, size_t length)
{
    while (length > 0U) {
        size_t available = SHA256_BLOCK_SIZE - context->block_length;
        size_t take = length < available ? length : available;
        memcpy(context->block + context->block_length, data, take);
        context->block_length += take;
        data += take;
        length -= take;
        if (context->block_length == SHA256_BLOCK_SIZE) {
            sha256_transform(context, context->block);
            context->bit_count += 512U;
            context->block_length = 0U;
        }
    }
}

static void sha256_final(Sha256Context *context, uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint64_t total_bits = context->bit_count + (uint64_t)context->block_length * 8U;
    size_t index;

    context->block[context->block_length++] = 0x80U;
    if (context->block_length > 56U) {
        memset(context->block + context->block_length, 0, SHA256_BLOCK_SIZE - context->block_length);
        sha256_transform(context, context->block);
        context->block_length = 0U;
    }
    memset(context->block + context->block_length, 0, 56U - context->block_length);
    for (index = 0U; index < 8U; ++index) {
        context->block[56U + index] = (uint8_t)(total_bits >> (56U - (unsigned int)index * 8U));
    }
    sha256_transform(context, context->block);
    for (index = 0U; index < 8U; ++index) {
        digest[index * 4U] = (uint8_t)(context->state[index] >> 24U);
        digest[index * 4U + 1U] = (uint8_t)(context->state[index] >> 16U);
        digest[index * 4U + 2U] = (uint8_t)(context->state[index] >> 8U);
        digest[index * 4U + 3U] = (uint8_t)context->state[index];
    }
}

int milena_sha256_file(const char *path, char digest_hex[65], uint64_t *size_out)
{
    static const char hex_digits[] = "0123456789abcdef";
    Sha256Context context;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t buffer[8192];
    uint64_t total_size = 0U;
    FILE *file;
    size_t count;
    size_t index;

    if (path == NULL || digest_hex == NULL) {
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    sha256_init(&context);
    while ((count = fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
        if (UINT64_MAX - total_size < (uint64_t)count) {
            (void)fclose(file);
            return -1;
        }
        total_size += (uint64_t)count;
        sha256_update(&context, buffer, count);
    }
    if (ferror(file) != 0) {
        (void)fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    sha256_final(&context, digest);
    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        digest_hex[index * 2U] = hex_digits[digest[index] >> 4U];
        digest_hex[index * 2U + 1U] = hex_digits[digest[index] & 0x0fU];
    }
    digest_hex[64] = '\0';
    if (size_out != NULL) {
        *size_out = total_size;
    }
    return 0;
}
