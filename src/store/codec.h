/* codec.h: gt_record <-> the 24 bytes on disk (layout in include/record.h).
 * Fields are serialized one by one in little-endian; struct layout is never written. */
#ifndef GT_CODEC_H
#define GT_CODEC_H

#include <stdbool.h>
#include <stdint.h>

#include "record.h"

/* Writes rec into out and fills the CRC32 (bytes 20..23) over bytes 0..19.
 * rec->crc32 is ignored. */
void record_encode(const gt_record* rec, uint8_t out[GT_RECORD_SIZE]);

/* Reads all fields, including the stored crc32, into rec.  Returns true when the stored
 * CRC32 matches the one computed over bytes 0..19. */
bool record_decode(const uint8_t in[GT_RECORD_SIZE], gt_record* rec);

#endif /* GT_CODEC_H */
