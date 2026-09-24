#include "group_key_codec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const unsigned char first_text[] = "north|\"quoted\"";
    static const unsigned char second_text[] = "a,b|c";
    unsigned char encoded[MILENA_GROUP_KEY_CODEC_MAX_BYTES];
    size_t encoded_length = 0;
    MilenaError error;
    MilenaGroupKeyTextPart first = {first_text, sizeof(first_text) - 1u, true};
    MilenaGroupKeyTextPart second = {second_text, sizeof(second_text) - 1u, true};
    MilenaGroupKeyTextPart decoded_first, decoded_second;

    milena_error_init(&error);
    assert(milena_group_key_encode_text_pair(first, second, encoded,
        sizeof(encoded), &encoded_length, &error) == MILENA_OK);
    assert(encoded_length > 6u);
    assert(encoded[3] == MILENA_GROUP_KEY_CODEC_VERSION);
    assert(encoded[4] == 2u);
    assert(milena_group_key_decode_text_pair(encoded, encoded_length,
        &decoded_first, &decoded_second, &error) == MILENA_OK);
    assert(decoded_first.valid && decoded_first.length == first.length);
    assert(decoded_second.valid && decoded_second.length == second.length);
    assert(memcmp(decoded_first.bytes, first.bytes, first.length) == 0);
    assert(memcmp(decoded_second.bytes, second.bytes, second.length) == 0);

    /* Empty text is valid, and cannot alias either a separator-containing value
     * or an explicitly invalid/null component. */
    first.bytes = NULL; first.length = 0; first.valid = true;
    second.bytes = (const unsigned char *)""; second.length = 0; second.valid = true;
    assert(milena_group_key_encode_text_pair(first, second, encoded,
        sizeof(encoded), &encoded_length, &error) == MILENA_OK);
    assert(milena_group_key_decode_text_pair(encoded, encoded_length,
        &decoded_first, &decoded_second, &error) == MILENA_OK);
    assert(decoded_first.valid && decoded_first.length == 0u);
    assert(decoded_second.valid && decoded_second.length == 0u);

    first.valid = false;
    assert(milena_group_key_encode_text_pair(first, second, encoded,
        sizeof(encoded), &encoded_length, &error) == MILENA_OK);
    assert(milena_group_key_decode_text_pair(encoded, encoded_length,
        &decoded_first, &decoded_second, &error) == MILENA_OK);
    assert(!decoded_first.valid && decoded_first.bytes == NULL);
    assert(decoded_second.valid);

    /* The encoding is length-prefixed, so ("ab", "c") and ("a", "bc")
     * produce different exact byte strings with no delimiter ambiguity. */
    {
        unsigned char key_ab_c[MILENA_GROUP_KEY_CODEC_MAX_BYTES];
        unsigned char key_a_bc[MILENA_GROUP_KEY_CODEC_MAX_BYTES];
        size_t len_ab_c = 0, len_a_bc = 0;
        MilenaGroupKeyTextPart ab = {(const unsigned char *)"ab", 2u, true};
        MilenaGroupKeyTextPart c = {(const unsigned char *)"c", 1u, true};
        MilenaGroupKeyTextPart a = {(const unsigned char *)"a", 1u, true};
        MilenaGroupKeyTextPart bc = {(const unsigned char *)"bc", 2u, true};
        assert(milena_group_key_encode_text_pair(ab, c, key_ab_c,
            sizeof(key_ab_c), &len_ab_c, &error) == MILENA_OK);
        assert(milena_group_key_encode_text_pair(a, bc, key_a_bc,
            sizeof(key_a_bc), &len_a_bc, &error) == MILENA_OK);
        assert(len_ab_c == len_a_bc);
        assert(memcmp(key_ab_c, key_a_bc, len_ab_c) != 0);
    }

    /* Strict corruption handling: unsupported version, truncation, trailing
     * bytes, invalid type, and nonempty null are never silently decoded. */
    encoded[3] = 9u;
    assert(milena_group_key_decode_text_pair(encoded, encoded_length,
        &decoded_first, &decoded_second, &error) == MILENA_ERR_DATA);
    encoded[3] = MILENA_GROUP_KEY_CODEC_VERSION;
    assert(milena_group_key_decode_text_pair(encoded, encoded_length - 1u,
        &decoded_first, &decoded_second, &error) == MILENA_ERR_DATA);
    encoded[encoded_length] = 0u;
    assert(milena_group_key_decode_text_pair(encoded, encoded_length + 1u,
        &decoded_first, &decoded_second, &error) == MILENA_ERR_DATA);
    encoded[encoded_length - 1u] = 2u;
    assert(milena_group_key_decode_text_pair(encoded, encoded_length,
        &decoded_first, &decoded_second, &error) == MILENA_ERR_DATA);

    /* Caller buffer limits and the codec's hard limit are both checked before
     * writing, with the output length left untouched on failure. */
    {
        unsigned char too_large[MILENA_GROUP_KEY_CODEC_MAX_BYTES + 1u];
        MilenaGroupKeyTextPart huge = {too_large, sizeof(too_large), true};
        size_t unchanged = 777u;
        assert(milena_group_key_encode_text_pair(huge, second, encoded,
            sizeof(encoded), &unchanged, &error) == MILENA_ERR_OVERFLOW);
        assert(unchanged == 777u);
        assert(milena_group_key_encode_text_pair(first, second, encoded,
            1u, &unchanged, &error) == MILENA_ERR_OVERFLOW);
    }

    puts("group key codec tests passed");
    return 0;
}
