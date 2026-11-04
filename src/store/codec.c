#include "codec.h"

#include "crc32.h"

#define CRC_OFFSET 20u /* the CRC32 covers bytes 0..19 */

static void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(uint8_t* p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t* p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static uint64_t get64(const uint8_t* p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

void record_encode(const gt_record* rec, uint8_t out[GT_RECORD_SIZE])
{
    put64(out + 0, rec->ts_unix_ms);
    put32(out + 8, rec->seq);
    put16(out + 12, rec->soil_raw);
    put16(out + 14, rec->flags);
    put16(out + 16, rec->light_bright_ms);
    put16(out + 18, rec->light_edges);
    put32(out + CRC_OFFSET, gt_crc32(out, CRC_OFFSET));
}

bool record_decode(const uint8_t in[GT_RECORD_SIZE], gt_record* rec)
{
    rec->ts_unix_ms = get64(in + 0);
    rec->seq = get32(in + 8);
    rec->soil_raw = get16(in + 12);
    rec->flags = get16(in + 14);
    rec->light_bright_ms = get16(in + 16);
    rec->light_edges = get16(in + 18);
    rec->crc32 = get32(in + CRC_OFFSET);
    return rec->crc32 == gt_crc32(in, CRC_OFFSET);
}
