#include "meter.h"

#include "drivers/i2c_bus.h"
#include "bsp/log.h"

// Low-pass coefficient 1/8: about 80ms time constant at 10ms sampling, so the displayed digits do not jump
#define LPF_SHIFT 3

// Consecutive read failures reaching this count are treated as offline; once offline, re-probe at intervals
#define FAIL_STREAK_OFFLINE 5
#define REPROBE_INTERVAL_MS 1000
#define REPROBE_SLOW_MS     10000   // Device-level faults (ADC not converting, etc.) are also retried slowly: 2026-10-26 measured NO_CONVERSION 0.7s after power-up, recovered after a soft reset

static const uint8_t ina_addr[CH_COUNT] = {INA_CH1_ADDR, INA_CH2_ADDR};
static const int8_t  current_sign[CH_COUNT] = {INA_CH1_CURRENT_SIGN, INA_CH2_CURRENT_SIGN};

static ina2xx_t   sensor[CH_COUNT];
static meter_ch_t ch_data[CH_COUNT];
static uint8_t    fail_streak[CH_COUNT];
static bool       sampled_once;
static uint32_t   last_sample_ms;
static uint32_t   last_probe_ms;
static uint32_t   last_slow_ms;

static int32_t lpf(int32_t avg, int32_t sample)
{
    return avg + ((sample - avg) >> LPF_SHIFT);
}

static bool probe(ch_id_t ch, bool verbose)
{
    bool ok = ina2xx_init(&sensor[ch], ina_addr[ch], INA_SHUNT_UOHM);
    if (ok || verbose)
    {
        LOG("CH%d %s @0x%02X %s mfg=0x%04X id=0x%04X cal=%u trim=%s i2c_err=%lu",
            ch + 1, ina2xx_chip_name(sensor[ch].chip), ina_addr[ch], ina2xx_fault_str(sensor[ch].fault),
            sensor[ch].manufacturer_id, sensor[ch].device_id, sensor[ch].shunt_cal,
            sensor[ch].trim_ok ? "OK" : "ERR", i2c_bus_error_count());
    }
    return ok;
}

// Only re-probe for faults that may recover on their own; device-level faults (ADC not converting, etc.) need a power cycle,
// and their initialization blocks for about 50ms, so repeated probing would slow down the main loop
static bool fault_retryable(ina2xx_fault_t fault)
{
    return fault == INA_FAULT_NONE || fault == INA_FAULT_NO_ACK || fault == INA_FAULT_CONFIG;
}

void meter_init(void)
{
    i2c_bus_init();

    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        ch_data[ch] = (meter_ch_t){0};
        fail_streak[ch] = 0;
        probe(ch, true);
    }
}

void meter_sample(uint32_t now_ms)
{
    uint32_t dt_ms = sampled_once ? (now_ms - last_sample_ms) : 0;
    sampled_once = true;
    last_sample_ms = now_ms;

    bool reprobe = (uint32_t)(now_ms - last_probe_ms) >= REPROBE_INTERVAL_MS;
    if (reprobe) last_probe_ms = now_ms;
    bool reprobe_slow = (uint32_t)(now_ms - last_slow_ms) >= REPROBE_SLOW_MS;
    if (reprobe_slow) last_slow_ms = now_ms;

    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        ina2xx_t *s = &sensor[ch];
        meter_ch_t *m = &ch_data[ch];

        if (!s->present)
        {
            m->online = false;
            if ((reprobe && fault_retryable(s->fault)) || (reprobe_slow && !fault_retryable(s->fault))) probe(ch, false);
            continue;
        }

        if (!ina2xx_read(s))
        {
            m->online = false;
            if (++fail_streak[ch] >= FAIL_STREAK_OFFLINE)
            {
                s->present = false;
                LOG("INA CH%d offline", ch + 1);
            }
            continue;
        }
        fail_streak[ch] = 0;

        bool was_online = m->online;
        m->online = true;
        m->vbus_mv = s->vbus_mv;
        int32_t current_ua = s->current_ua * current_sign[ch];
        m->current_ma = current_ua / 1000;
        m->power_mw = s->vbus_mv * m->current_ma / 1000;      // mV × mA fits in 32 bits (≤ 60V × 16A)
        m->temp_mc = s->temp_mc;

        if (!was_online)
        {
            m->vbus_mv_avg = m->vbus_mv;
            m->current_ma_avg = m->current_ma;
            m->power_mw_avg = m->power_mw;
            continue;   // The first sample after coming back online is not counted in the accumulation
        }

        m->vbus_mv_avg = lpf(m->vbus_mv_avg, m->vbus_mv);
        m->current_ma_avg = lpf(m->current_ma_avg, m->current_ma);
        m->power_mw_avg = lpf(m->power_mw_avg, m->power_mw);

        if (m->current_ma > 0)
        {
            m->energy_mwms += (uint64_t)m->power_mw * dt_ms;
            m->charge_mams += (uint64_t)m->current_ma * dt_ms;
        }
    }
}

const meter_ch_t *meter_get(ch_id_t ch)
{
    return &ch_data[ch];
}

void meter_restore_energy(ch_id_t ch, uint64_t energy_mwms, uint64_t charge_mams)
{
    if (ch >= CH_COUNT) return;
    ch_data[ch].energy_mwms = energy_mwms;
    ch_data[ch].charge_mams = charge_mams;
}

// mW·ms -> mWh (÷ 3.6e6 = ÷ 2^7 ÷ 28125), using only 32-bit division: divide the high 32 bits separately, long-divide the low 32 bits in two 16-bit halves
int32_t meter_mwms_to_mwh(uint64_t mwms)
{
    const uint32_t d = 28125;
    uint64_t n = mwms >> 7;
    uint32_t hi = (uint32_t)(n >> 32), lo = (uint32_t)n;
    uint32_t q_hi = hi / d, r = hi % d;
    uint32_t t = (r << 16) | (lo >> 16);
    uint32_t q1 = t / d;
    r = t % d;
    t = (r << 16) | (lo & 0xFFFF);
    uint32_t q0 = t / d;
    uint64_t q = ((uint64_t)q_hi << 32) | ((uint64_t)q1 << 16) | q0;
    return (q > 0x7FFFFFFF) ? 0x7FFFFFFF : (int32_t)q;
}

const ina2xx_t *meter_get_sensor(ch_id_t ch)
{
    return &sensor[ch];
}

int32_t meter_total_power_mw(void)
{
    int32_t sum = 0;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
        if (ch_data[ch].online) sum += ch_data[ch].power_mw_avg;
    return sum;
}

int32_t meter_total_current_ma(void)
{
    int32_t sum = 0;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
        if (ch_data[ch].online) sum += ch_data[ch].current_ma_avg;
    return sum;
}
