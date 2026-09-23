#include "group_key_codec.h"

#include <string.h>

#define CODEC_HEADER_BYTES 6u
#define CODEC_COMPONENT_HEADER_BYTES 6u
#define CODEC_COMPONENT_TEXT 1u
#define CODEC_VALID 1u
#define CODEC_INVALID 0u

static void codec_u32_write(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static uint32_t codec_u32_read(const unsigned char *in) {
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static MilenaStatus codec_error(MilenaError *error, MilenaStatus status,
                                const char *message) {
    milena_error_set(error, status, 0, 0, 0, message);
    return status;
}

static bool codec_part_size(MilenaGroupKeyTextPart part, size_t *size) {
    if ((!part.valid && (part.length != 0 || part.bytes != NULL)) ||
        (part.valid && part.length != 0 && part.bytes == NULL) ||
        part.length > UINT32_MAX) return false;
    *size = CODEC_COMPONENT_HEADER_BYTES;
    return milena_size_add(*size, part.length, size);
}

static void codec_write_part(unsigned char *out, MilenaGroupKeyTextPart part) {
    out[0] = CODEC_COMPONENT_TEXT;
    out[1] = part.valid ? CODEC_VALID : CODEC_INVALID;
    codec_u32_write(out + 2, (uint32_t)part.length);
    if (part.length) memcpy(out + CODEC_COMPONENT_HEADER_BYTES,
                             part.bytes, part.length);
}

MilenaStatus milena_group_key_encode_text_pair(
    MilenaGroupKeyTextPart first, MilenaGroupKeyTextPart second,
    unsigned char *out, size_t out_capacity, size_t *out_length,
    MilenaError *error) {
    size_t first_size = 0, second_size = 0, total = CODEC_HEADER_BYTES;
    if (!out || !out_length || !codec_part_size(first, &first_size) ||
        !codec_part_size(second, &second_size) ||
        !milena_size_add(total, first_size, &total) ||
        !milena_size_add(total, second_size, &total)) {
        return codec_error(error, MILENA_ERR_ARGUMENT,
                           "Componentes inválidos para clave compuesta texto");
    }
    if (total > MILENA_GROUP_KEY_CODEC_MAX_BYTES || total > out_capacity) {
        return codec_error(error, MILENA_ERR_OVERFLOW,
                           "La clave compuesta supera el límite de bytes del codec");
    }
    out[0] = 'M'; out[1] = 'G'; out[2] = 'K';
    out[3] = (unsigned char)MILENA_GROUP_KEY_CODEC_VERSION;
    out[4] = (unsigned char)2u; /* fixed arity */
    out[5] = 0u; /* reserved, must remain zero in version 1 */
    codec_write_part(out + CODEC_HEADER_BYTES, first);
    codec_write_part(out + CODEC_HEADER_BYTES + first_size, second);
    *out_length = total;
    milena_error_clear(error);
    return MILENA_OK;
}

static bool codec_read_part(const unsigned char *encoded, size_t remaining,
                            MilenaGroupKeyTextPart *part, size_t *consumed) {
    uint32_t length;
    if (remaining < CODEC_COMPONENT_HEADER_BYTES ||
        encoded[0] != CODEC_COMPONENT_TEXT ||
        encoded[1] > CODEC_VALID) return false;
    length = codec_u32_read(encoded + 2);
    if (encoded[1] == CODEC_INVALID && length != 0u) return false;
    if ((size_t)length > remaining - CODEC_COMPONENT_HEADER_BYTES) return false;
    part->valid = encoded[1] == CODEC_VALID;
    part->length = (size_t)length;
    part->bytes = part->valid ? encoded + CODEC_COMPONENT_HEADER_BYTES : NULL;
    *consumed = CODEC_COMPONENT_HEADER_BYTES + (size_t)length;
    return true;
}

MilenaStatus milena_group_key_decode_text_pair(
    const unsigned char *encoded, size_t encoded_length,
    MilenaGroupKeyTextPart *first, MilenaGroupKeyTextPart *second,
    MilenaError *error) {
    size_t offset = CODEC_HEADER_BYTES, consumed = 0;
    if (!encoded || !first || !second || encoded_length < CODEC_HEADER_BYTES ||
        encoded_length > MILENA_GROUP_KEY_CODEC_MAX_BYTES) {
        return codec_error(error, MILENA_ERR_ARGUMENT,
                           "Clave compuesta ausente o fuera del límite del codec");
    }
    if (encoded[0] != 'M' || encoded[1] != 'G' || encoded[2] != 'K' ||
        encoded[3] != MILENA_GROUP_KEY_CODEC_VERSION || encoded[4] != 2u ||
        encoded[5] != 0u) {
        return codec_error(error, MILENA_ERR_DATA,
                           "Versión o forma no reconocida de clave compuesta");
    }
    if (!codec_read_part(encoded + offset, encoded_length - offset,
                         first, &consumed)) {
        return codec_error(error, MILENA_ERR_DATA,
                           "Primer componente inválido en clave compuesta");
    }
    offset += consumed;
    if (!codec_read_part(encoded + offset, encoded_length - offset,
                         second, &consumed)) {
        return codec_error(error, MILENA_ERR_DATA,
                           "Segundo componente inválido en clave compuesta");
    }
    offset += consumed;
    if (offset != encoded_length) {
        return codec_error(error, MILENA_ERR_DATA,
                           "Bytes sobrantes en clave compuesta");
    }
    milena_error_clear(error);
    return MILENA_OK;
}
