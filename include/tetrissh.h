#ifndef TETRISH_SSH_H
#define TETRISH_SSH_H

#include <stdint.h>

uint32_t decode_u32_be(const uint8_t buf[4]);
void encode_u32_be(uint8_t buf[4], uint32_t value);


#endif

