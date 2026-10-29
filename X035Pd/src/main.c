// X035Pd: USB PD Sink slave firmware
//   Only does PD negotiation and message forwarding; all policy such as target voltage and mode is issued by the main controller V203 over UART (docs/PROTOCOL.md)

#include "ch32x035.h"
#include "config.h"
#include "link.h"
#include "nv.h"
#include "sys_time.h"
#include "usbpd_pe.h"
#include "usbpd_phy.h"
#include "usbpd_tc.h"
#include "vbus.h"

static void iwdg_init(void)
{
    // LSI about 128kHz, /32 prescaler, reload 4000 -> about 1s
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_32);
    IWDG_SetReload(4000);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    sys_time_init();

    nv_init();
    vbus_init();
    phy_init();
    pe_init();
    tc_init();
    link_init();
    iwdg_init();

    uint32_t vbus_at = 0, tc_at = 0;
    while (1)
    {
        uint32_t now = sys_time_ms();

        if ((uint32_t)(now - vbus_at) >= CFG_VBUS_SAMPLE_MS)
        {
            vbus_at = now;
            vbus_sample();
        }
        if ((uint32_t)(now - tc_at) >= CFG_TC_PERIOD_MS)
        {
            tc_at = now;
            tc_run(now);
        }
        pe_run(now);
        link_process(now);
        IWDG_ReloadCounter();
    }
}
