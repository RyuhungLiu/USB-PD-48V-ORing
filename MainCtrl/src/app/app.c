#include "app.h"

#include <stdbool.h>
#include "app/settings.h"
#include "bsp/log.h"
#include "pd/pd_node.h"
#include "power/oring.h"
#include "power/share.h"

// After the previous channel finishes switching, wait for VBUS to settle before switching the next one
#define VSEQ_SETTLE_MS  1000

// Switch voltage one channel at a time: 3V3 (main controller and both X035s) is powered from both VBUS rails via diodes D3/D4,
// a Hard Reset on both channels at once drops both VBUS to zero for about 1s and the whole board loses power. Switching one at a time keeps the other channel powering the board
static struct
{
    uint8_t  pending;       // bitmap of channels waiting to switch
    int8_t   active;        // channel currently switching, -1 = none
    uint32_t settle_since;  // 0 = settle wait not started
} vseq = {.active = -1};

static bool preset_checked[CH_COUNT];

void app_init(void)
{
    settings_init();

    const settings_t *s = settings_get();
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        preset_checked[ch] = false;
        if (s->auto_on && s->target_mv[ch]) oring_arm(ch, s->target_mv[ch]);
    }
}

static void vseq_start_next(void)
{
    while (vseq.pending)
    {
        ch_id_t ch = (vseq.pending & (1u << CH1)) ? CH1 : CH2;
        vseq.pending &= ~(1u << ch);

        // The voltage is always that channel's saved target (both user settings and the power-on push are written into settings first)
        if (pd_node_set_voltage(ch, settings_get()->target_mv[ch]))
        {
            vseq.active = (int8_t)ch;
            vseq.settle_since = 0;
            return;
        }
        LOG("APP CH%d vset skipped: PD link not ready", ch + 1);
    }
    vseq.active = -1;
}

static void vseq_process(uint32_t now_ms)
{
    if (vseq.active < 0) return;

    if (pd_node_get((ch_id_t)vseq.active)->vset == PD_VSET_BUSY)
    {
        vseq.settle_since = 0;
        return;
    }
    // After completion (OK / FAILED / STORED), wait for VBUS to settle again
    if (!vseq.settle_since)
    {
        vseq.settle_since = now_ms ? now_ms : 1;
        return;
    }
    if ((uint32_t)(now_ms - vseq.settle_since) < VSEQ_SETTLE_MS) return;

    vseq.active = -1;
    vseq_start_next();
}

uint8_t app_set_voltage(uint8_t ch_mask, uint16_t mv)
{
    uint8_t accepted = 0;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        if (!(ch_mask & (1u << ch)) || !pd_node_ready(ch)) continue;
        accepted |= 1u << ch;
        oring_request(ch, false);   // turn off and cancel any pending auto-recovery, so the voltage jump is not applied directly to the load
        settings_set_target(ch, mv);
    }
    if (!accepted) return 0;

    share_abort("VOLTAGE CHANGED");

    // Write first, then send: if this channel is the only power source, the whole board loses power during Hard Reset and this value applies after power returns
    settings_save_now();

    vseq.pending |= accepted;
    if (vseq.active < 0) vseq_start_next();
    LOG("APP set %umV mask=0x%X, one channel at a time", mv, accepted);
    return accepted;
}

bool app_vset_busy(void)
{
    return vseq.active >= 0 || vseq.pending;
}

bool app_vset_queued(ch_id_t ch)
{
    return ch < CH_COUNT && (vseq.pending & (1u << ch));
}

// Each channel is checked only once, when its X035 status is first received; mismatching channels are added to the one-at-a-time switch queue (only while output is off)
static void check_preset(ch_id_t ch)
{
    uint16_t target = settings_get()->target_mv[ch];

    if (!target)
    {
        preset_checked[ch] = true;
        return;
    }
    if (!pd_node_ready(ch)) return;

    preset_checked[ch] = true;
    if (pd_node_preset_matches(ch, target)) return;
    if (oring_path_is_enabled(ch))
    {
        LOG("APP CH%d preset %umV != target %umV, output on: skip push", ch + 1, pd_node_get(ch)->preset_mv, target);
        return;
    }
    LOG("APP CH%d preset %umV != target %umV: push", ch + 1, pd_node_get(ch)->preset_mv, target);
    vseq.pending |= 1u << ch;
    if (vseq.active < 0) vseq_start_next();
}

void app_process(uint32_t now_ms)
{
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        if (!preset_checked[ch]) check_preset(ch);
    }
    vseq_process(now_ms);
    settings_process(now_ms);
}
