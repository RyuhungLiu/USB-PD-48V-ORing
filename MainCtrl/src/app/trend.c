#include "trend.h"

#include "board.h"
#include "power/meter.h"

typedef struct trend_buf_t
{
    trend_pt_t pt[TREND_LEN];
    uint32_t   seq;                 // total points written; index of next point = seq % TREND_LEN
} trend_buf_t;

static trend_buf_t buf[TREND_SCALE_COUNT];
static int32_t acc1, acc2;
static uint8_t acc_n;
static uint32_t last_ms;
static bool     started;

static int16_t power_d10(ch_id_t ch)
{
    const meter_ch_t *m = meter_get(ch);
    if (!m->online || m->power_mw_avg <= 0) return 0;
    int32_t v = (m->power_mw_avg + 50) / 100;
    return (int16_t)(v > 32767 ? 32767 : v);
}

static void push(trend_scale_t s, int16_t p1, int16_t p2)
{
    trend_buf_t *b = &buf[s];
    b->pt[b->seq % TREND_LEN] = (trend_pt_t){p1, p2};
    b->seq++;
}

void trend_sample(uint32_t now_ms)
{
    if (started && (uint32_t)(now_ms - last_ms) < TREND_FINE_MS) return;
    // Advance in fixed steps; if several periods behind, sample only once and do not backfill
    last_ms = (started && (uint32_t)(now_ms - last_ms) < 2 * TREND_FINE_MS) ? last_ms + TREND_FINE_MS : now_ms;
    started = true;

    int16_t p1 = power_d10(CH1), p2 = power_d10(CH2);
    push(TREND_FINE, p1, p2);

    acc1 += p1;
    acc2 += p2;
    if (++acc_n >= TREND_COARSE_DIV)
    {
        push(TREND_COARSE, (int16_t)(acc1 / acc_n), (int16_t)(acc2 / acc_n));
        acc1 = acc2 = 0;
        acc_n = 0;
    }
}

uint16_t trend_count(trend_scale_t scale)
{
    uint32_t n = buf[scale].seq;
    return (uint16_t)(n < TREND_LEN ? n : TREND_LEN);
}

trend_pt_t trend_get(trend_scale_t scale, uint16_t i)
{
    const trend_buf_t *b = &buf[scale];
    uint32_t oldest = (b->seq < TREND_LEN) ? 0 : b->seq - TREND_LEN;
    return b->pt[(oldest + i) % TREND_LEN];
}

uint32_t trend_seq(trend_scale_t scale)
{
    return buf[scale].seq;
}

uint16_t trend_period_ms(trend_scale_t scale)
{
    return (scale == TREND_FINE) ? TREND_FINE_MS : TREND_FINE_MS * TREND_COARSE_DIV;
}
