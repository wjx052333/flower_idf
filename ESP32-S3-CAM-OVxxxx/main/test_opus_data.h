#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Pre-encoded Opus test utterances.
 *
 * Format: flat sequence of [u16_le frame_len][opus_payload]...
 * Read with: len = data[pos] | (data[pos+1] << 8); frame = &data[pos+2]; pos += 2 + len;
 */

extern const uint8_t test_opus_tools[];
extern const size_t  test_opus_tools_size;

extern const uint8_t test_opus_multiturn_0[];
extern const size_t  test_opus_multiturn_0_size;
extern const uint8_t test_opus_multiturn_1[];
extern const size_t  test_opus_multiturn_1_size;
extern const uint8_t test_opus_multiturn_2[];
extern const size_t  test_opus_multiturn_2_size;
