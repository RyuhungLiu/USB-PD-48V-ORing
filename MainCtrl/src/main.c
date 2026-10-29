#include <ch32v20x.h>
#include <stdbool.h>
#include "board.h"
#include "config.h"
#include "bsp/sys.h"
#include "bsp/delay.h"
#include "bsp/iwdg.h"
#include "bsp/log.h"
#include "bsp/resetlog.h"
#include "power/oring.h"
#include "power/meter.h"
#include "power/share.h"
#include "pd/pd_node.h"
#include "drivers/st7789.h"
#include "ui/gfx.h"
#include "drivers/key.h"
#include "ui/ui.h"
#include "diag/baud_sweep.h"
#include "app/app.h"
#include "app/dbgcmd.h"
#include "app/stats.h"
#include "app/trend.h"

// Returns true when due and advances the timestamp; if several periods behind, runs only once without catching up
static inline bool task_due(uint32_t now, uint32_t *last, uint32_t period)
{
    if ((uint32_t)(now - *last) < period) return false;

    *last += period;
    if ((uint32_t)(now - *last) >= period) *last = now;
    return true;
}

static void log_meter(void)
{
#if ENABLE_SDI_LOG
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        const pd_node_t *n = pd_node_get(ch);
        LOG("PD%d %s rx=%lu bad=%lu cc=%u vbus=%umV%s pe=%u", ch + 1, pd_node_link_str(n->link), n->rx_frames,
            n->bad_frames, n->cc, n->vbus_mv, pd_node_vbus_fresh(ch, millis()) ? "" : "(stale)", n->pe_state);
        LOG("PD%d contract=%s %s #%u %umV %umA vset=%s target=%umV preset=%umV", ch + 1,
            n->contract_valid ? (n->epr ? "EPR" : "SPR") : "none", pd_pdo_kind_str(n->contract_kind),
            n->contract_pos, n->contract_mv, n->contract_ma, pd_vset_str(n->vset), n->vset_target_mv, n->preset_mv);
        LOG("PD%d fw %u.%u.%u boots=%lu cmd=0x%02X %s", ch + 1, n->fw_major, n->fw_minor, n->fw_patch,
            n->x035_boots, n->last_cmd_code, pd_result_str(n->last_cmd_result));

        const meter_ch_t *m = meter_get(ch);
        if (!m->online)
        {
            LOG("CH%d INA %s", ch + 1, ina2xx_fault_str(meter_get_sensor(ch)->fault));
            continue;
        }
        const char *trip = oring_last_trip(ch);
        LOG("CH%d V=%ldmV I=%ldmA P=%ldmW T=%ldmC path=%s%s ocp=%ldmA hr=%lu trip=%s", ch + 1,
            m->vbus_mv_avg, m->current_ma_avg, m->power_mw_avg, m->temp_mc,
            oring_path_is_enabled(ch) ? "ON" : "OFF",
            oring_fault_latched(ch) ? "(LATCHED)" : oring_wants_on(ch) && !oring_path_is_enabled(ch) ? "(ARM)" : "",
            oring_ocp_limit_ma(ch), n->hard_reset_count, trip ? trip : "-");
    }
#endif
}

int main(void)
{
    // Executed first: force both outputs off
    oring_init();

    sys_init();
    resetlog_init(sys_get_reset_cause());
    delay_init();
    bool warm = oring_warm_start();     // warm reset: VOUT not yet discharged, immediately restore channels conducting before reset
    bool kept = oring_restore_latch();  // warm reset: keep latched faults from before the reset
    LOG_INIT();

    LOG("MainCtrl v%d.%d.%d  SYSCLK=%luHz  reset=%s",
        CONFIG_FW_VERSION_MAJOR, CONFIG_FW_VERSION_MINOR, CONFIG_FW_VERSION_PATCH,
        SystemCoreClock, sys_reset_cause_str(sys_get_reset_cause()));
    const rl_prev_t *rl = resetlog_prev();
    if (rl->valid)
        LOG("RESET #%lu: last stage %s at %lums", rl->resets, resetlog_stage_str(rl->stage), rl->ms);
    LOG("ORING hw: %s (detected %s, polarity cfg %d)", oring_hw_str(oring_hw()),
        oring_hw_str(oring_hw_detected()), BOARD_ORING_POLARITY);
    if (warm) LOG("ORING warm start: CH1 %s CH2 %s", oring_path_is_enabled(CH1) ? "ON" : "-",
                  oring_path_is_enabled(CH2) ? "ON" : "-");
    if (kept) LOG("ORING latch kept: CH1 %s CH2 %s", oring_last_trip(CH1) ? oring_last_trip(CH1) : "-",
                  oring_last_trip(CH2) ? oring_last_trip(CH2) : "-");

    // Enable UART receive as early as possible: the X035 powers up at the same time, PD negotiation completes within a few hundred ms and is forwarded message by message,
    // if receive were enabled only after the LCD (about 0.45s delay) and INA initialization, all Source_Cap/Request/PS_RDY would be lost.
    // Messages are stored in the DMA circular buffer first and parsed after entering the main loop
    pd_node_init();

    lcd_init();
    meter_init();

    app_init();     // parameter storage: restore settings and accumulated energy
    share_init();
    stats_init();
    key_init();
    ui_init();

    iwdg_init(IWDG_TIMEOUT_MS);

    uint32_t now = millis();
    uint32_t t_key = now, t_meter = now, t_ui = now, t_slow = now;
#if DIAG_BAUD_SWEEP
    baud_sweep_init(now);
#endif

    while (1)
    {
        now = millis();
        resetlog_mark(RL_STAGE_PD, now);

#if DIAG_BAUD_SWEEP
        baud_sweep_process(now);   // diagnostic: replaces normal PD link processing
#else
        pd_node_process(now);
#endif

        if (task_due(now, &t_key, TASK_KEY_PERIOD_MS))
        {
            resetlog_mark(RL_STAGE_KEY, now);
            key_scan(now);
        }

        if (task_due(now, &t_meter, TASK_METER_PERIOD_MS))
        {
            // Every WCH-Link connection changes the PLL to x15 (UART baud rate -16%); checking every 10ms
            // limits garbled time to within 10ms (a single register read, negligible overhead normally)
            resetlog_mark(RL_STAGE_SLOW, now);
            if (sys_clock_check_repair())
                LOG("CLOCK FAULT #%lu: CFGR0=0x%08lX -> repaired to 0x%08lX", sys_clock_repairs(),
                    sys_clock_last_bad_cfgr0(), sys_clock_cfgr0());

            resetlog_mark(RL_STAGE_METER, now);
            meter_sample(now);
            resetlog_mark(RL_STAGE_ORING, now);
            oring_process(now);   // interlock, protection, auto-recovery
            share_process(now);   // current sharing
            stats_sample(now);
            trend_sample(now);
            resetlog_mark(RL_STAGE_APP, now);
            app_process(now);     // one-at-a-time voltage switching, parameter saving
            dbg_cmd_process(now); // debug command mailbox (WCH-Link, tools/v203ctl.py)
        }

        if (task_due(now, &t_ui, TASK_UI_PERIOD_MS))
        {
            resetlog_mark(RL_STAGE_UI, now);
            ui_process(now);
        }

        if (task_due(now, &t_slow, TASK_SLOW_PERIOD_MS))
        {
            resetlog_mark(RL_STAGE_SLOW, now);
            log_meter();
        }

        resetlog_mark(RL_STAGE_IDLE, now);
        iwdg_feed();
    }
}
