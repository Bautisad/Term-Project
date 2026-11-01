#include "soil_logic.h"

uint16_t soil_median(const uint16_t* v, size_t n)
{
    uint16_t s[SOIL_MAX_SAMPLES];

    if (v == NULL || n == 0 || n > SOIL_MAX_SAMPLES) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) { /* insertion sort of a copy */
        uint16_t x = v[i];
        size_t   j = i;

        while (j > 0 && s[j - 1] > x) {
            s[j] = s[j - 1];
            j--;
        }
        s[j] = x;
    }
    return s[n / 2];
}

uint16_t soil_judge(soil_state* st, const soil_cfg* cfg, bool read_ok, uint16_t value)
{
    uint16_t status = GT_ST_OK;

    if (!read_ok) {
        return GT_ST_INVALID;
    }
    if (st->have_last && st->last == value) {
        if (st->repeat < ~0u) {
            st->repeat++;
        }
    } else {
        st->repeat = 1;
    }
    st->last = value;
    st->have_last = true;

    if (value < cfg->lo || value > cfg->hi) {
        status |= GT_ST_INVALID;
    }
    if (st->repeat >= cfg->stuck_n) {
        status |= GT_ST_STUCK;
    }
    return status;
}
