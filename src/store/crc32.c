#include "crc32.h"

/* Bitwise, so there is no table and no static state.  A record is 20 bytes at one
 * append a minute, so speed does not matter. */
uint32_t gt_crc32(const uint8_t* buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}
