/* record.h: on-disk record layout.  DRAFT: shared contract, needs both owners' sign-off.
 *
 * One record per reading cycle.  The store writes it, analysis reads it, so any
 * change here is a format change.  All multi-byte fields are little-endian on disk;
 * the store serializes field by field and must not rely on struct layout in memory.
 *
 *   offset  size  field
 *        0     8  ts_unix_ms        wall-clock milliseconds since the Unix epoch
 *        8     4  seq               sequence number, +1 per cycle; gaps mean the Pi was down
 *       12     2  soil_raw          STEMMA soil sensor capacitive value, 16-bit, stored raw
 *                                   (not a calibrated percent, so we can recalibrate later)
 *       14     2  flags             GT_REC_* bits below
 *       16     2  light_bright_ms   milliseconds the LM393 flag read "bright" during the cycle
 *       18     2  light_edges       bright/dark transitions seen during the cycle (saturates)
 *       20     4  crc32             CRC32 over bytes 0..19
 *
 * light_bright_ms is a uint16, so a cycle must not exceed 65535 ms; the planned cycle is
 * 60000 ms.  A longer cycle is a format change.
 */
#ifndef GT_RECORD_H
#define GT_RECORD_H

#include <stdint.h>

#define GT_RECORD_SIZE 24u

/* Flags.  A *_INVALID reading is never a real measurement: the matching value field
 * is stored as 0 and analysis must skip it. */
#define GT_REC_SOIL_INVALID  0x0001u /* out of range, or I2C read failed */
#define GT_REC_SOIL_STUCK    0x0002u /* identical value for too many samples; value kept */
#define GT_REC_LIGHT_INVALID 0x0004u /* flag stuck for too long, or chattering */
#define GT_REC_LIGHT_BRIGHT  0x0008u /* level at the end of the cycle (1 = bright) */
#define GT_REC_TIME_UNSET    0x0010u /* wall clock earlier than the last record or build time */
#define GT_REC_UNDERVOLT     0x0020u /* Pi reported under-voltage during the cycle */

typedef struct gt_record {
    uint64_t ts_unix_ms;
    uint32_t seq;
    uint16_t soil_raw;
    uint16_t flags;
    uint16_t light_bright_ms;
    uint16_t light_edges;
    uint32_t crc32;
} gt_record;

_Static_assert(sizeof(gt_record) == GT_RECORD_SIZE, "gt_record must be exactly 24 bytes");

#endif /* GT_RECORD_H */
