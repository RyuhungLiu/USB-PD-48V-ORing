#include "stats.h"

#include <string.h>
#include "config.h"
#include "app/settings.h"
#include "pd/pd_node.h"
#include "power/meter.h"
#include "power/oring.h"
#include "power/share.h"

static const char *const trip_names[TRIP_KIND_COUNT] = {
    [TRIP_OCP]          = "OCP",
    [TRIP_SCP]          = "SCP",
    [TRIP_REVERSE]      = "REVERSE",
    [TRIP_OTP]          = "OTP",
    [TRIP_OVP]          = "OVP",
    [TRIP_VOLTAGE]      = "VOLTAGE",
    [TRIP_DETACH]       = "DETACH",
    [TRIP_HARD_RESET]   = "HARD RESET",
    [TRIP_LINK_LOST]    = "PD LINK LOST",
    [TRIP_ISENSE_LOST]  = "I-SENSE LOST",
    [TRIP_CONTRACT]     = "PD CONTRACT",
    [TRIP_VSET]         = "VSET",
    [TRIP_OTHER]        = "OTHER",
};

static stats_t st;
static uint32_t last_ms;
static bool     sampled;

// Accumulated: base read from the parameter record at power-on + increment of this session
static uint32_t life_boots;
static uint32_t life_on_base_s;
static uint32_t life_on_ms;             // this power-on: cumulative time any channel conducted
static uint32_t life_trips_base;
static uint32_t life_trips;             // this power-on: number of trips

void stats_init(void)
{
    uint32_t boots, on_s, trips;
    settings_get_lifetime(&boots, &on_s, &trips);
    life_boots = boots + 1;
    life_on_base_s = on_s;
    life_trips_base = trips;
    settings_request_save();            // write the new boot count

    st = (stats_t){0};
}

void stats_reset_session(uint32_t now_ms)
{
    st = (stats_t){0};
    st.since_ms = now_ms;
}

void stats_sample(uint32_t now_ms)
{
    uint32_t dt = sampled ? (now_ms - last_ms) : 0;
    sampled = true;
    last_ms = now_ms;

    int32_t total_mw = 0;
    bool any_on = false;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        stats_ch_t *c = &st.ch[ch];
        if (oring_path_is_enabled(ch))
        {
            c->on_ms += dt;
            any_on = true;
        }

        const meter_ch_t *m = meter_get(ch);
        if (!m->online) continue;

        // Voltage sag: recorded only while conducting with a contract (not while off or during voltage switching)
        const pd_node_t *n = pd_node_get(ch);
        if (oring_path_is_enabled(ch) && n->contract_valid && n->contract_mv)
        {
            uint16_t v = (uint16_t)(m->vbus_mv > 0 ? m->vbus_mv : 0);
            if (!c->min_mv || v < c->min_mv)
            {
                c->min_mv = v;
                c->min_ma = (int16_t)m->current_ma;
                c->min_contract_mv = n->contract_mv;
                c->min_ms = now_ms;
            }
            if ((uint32_t)v * 100 < (uint32_t)n->contract_mv * STATS_SAG_PCT)
            {
                if (!c->sag_since)
                {
                    c->sag_since = now_ms ? now_ms : 1;
                    c->sag_count++;
                }
                uint32_t d = now_ms - c->sag_since;
                if (d > c->sag_max_ms) c->sag_max_ms = (uint16_t)(d > 0xFFFF ? 0xFFFF : d);
            }
            else
            {
                c->sag_since = 0;
            }
        }
        else
        {
            c->sag_since = 0;
        }

        if (m->current_ma > c->peak_ma) c->peak_ma = m->current_ma;
        if (m->power_mw > c->peak_mw) c->peak_mw = m->power_mw;
        if (m->temp_mc > c->max_temp_mc) c->max_temp_mc = m->temp_mc;
        total_mw += m->power_mw;
    }
    if (total_mw > st.peak_total_mw) st.peak_total_mw = total_mw;
    if (any_on) life_on_ms += dt;
    if (share_state() == SHARE_RUN) st.share_run_ms += dt;
}

static stats_trip_kind_t classify(const char *reason)
{
    for (uint8_t k = 0; k < TRIP_OTHER; k++)
    {
        if (reason && strcmp(reason, trip_names[k]) == 0) return (stats_trip_kind_t)k;
    }
    return TRIP_OTHER;
}

void stats_on_trip(ch_id_t ch, const char *reason, bool latched, int32_t value, const char *unit, uint32_t now_ms)
{
    if (ch >= CH_COUNT) return;
    stats_ch_t *c = &st.ch[ch];
    c->trip_count[classify(reason)]++;

    // Voltage-type trips (VIN LOST / VOLTAGE): protection already tripped within the same cycle and stats_sample no longer sees this sample, so the minimum voltage is recorded here
    if (unit && unit[1] == 'V' && value > 0 && value < 0xFFFF && (!c->min_mv || value < c->min_mv))
    {
        c->min_mv = (uint16_t)value;
        c->min_ma = (int16_t)meter_get(ch)->current_ma;
        c->min_contract_mv = pd_node_get(ch)->contract_mv;
        c->min_ms = now_ms;
    }
    if (latched) c->trips_latched++;
    else c->trips_auto++;

    st.last_valid = true;
    st.last_ch = ch;
    st.last_reason = reason;
    st.last_value = value;
    st.last_unit = unit;
    st.last_latched = latched;
    st.last_ms = now_ms;

    life_trips++;
    settings_request_save();
}

const stats_t *stats_get(void)
{
    return &st;
}

const char *stats_trip_kind_str(stats_trip_kind_t kind)
{
    return (kind < TRIP_KIND_COUNT) ? trip_names[kind] : "?";
}

uint32_t stats_life_boots(void)
{
    return life_boots;
}

uint32_t stats_life_on_s(void)
{
    return life_on_base_s + life_on_ms / 1000;
}

uint32_t stats_life_trips(void)
{
    return life_trips_base + life_trips;
}
