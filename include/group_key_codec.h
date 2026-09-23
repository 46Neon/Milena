#ifndef MILENA_GROUP_KEY_CODEC_H
#define MILENA_GROUP_KEY_CODEC_H

#include "common.h"

/* Versioned, bounded product wire representation for two TEXT group-key
 * components. Payloads are borrowed UTF-8 bytes; this codec never interprets
 * or normalizes text. The stream caller validates UTF-8 and separately
 * enforces its policy cap. */
#define MILENA_GROUP_KEY_CODEC_VERSION 1u
#define MILENA_GROUP_KEY_CODEC_MAX_BYTES 4096u

typedef struct {
    const unsigned char *bytes;
    size_t length;
    bool valid;
} MilenaGroupKeyTextPart;

/* Encode two typed text components. A valid empty string is distinct from an
 * invalid/null component. Invalid components must have length zero. `out_length`
 * is written only on success. */
MilenaStatus milena_group_key_encode_text_pair(
    MilenaGroupKeyTextPart first, MilenaGroupKeyTextPart second,
    unsigned char *out, size_t out_capacity, size_t *out_length,
    MilenaError *error);

/* Decode a complete version-1 pair without allocating. Returned byte spans
 * point into `encoded`; malformed or trailing bytes are rejected. */
MilenaStatus milena_group_key_decode_text_pair(
    const unsigned char *encoded, size_t encoded_length,
    MilenaGroupKeyTextPart *first, MilenaGroupKeyTextPart *second,
    MilenaError *error);

#endif
