/* crc32.h: CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) used by every record.
 * Private to the store, but tools may include it so they check records with the same code. */
#ifndef GT_CRC32_H
#define GT_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 of buf[0..len).  The check value for "123456789" is 0xCBF43926. */
uint32_t gt_crc32(const uint8_t* buf, size_t len);

#endif /* GT_CRC32_H */
