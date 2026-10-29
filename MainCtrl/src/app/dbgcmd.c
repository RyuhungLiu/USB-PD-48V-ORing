#include "dbgcmd.h"

#include <ch32v20x.h>

#include "app/app.h"
#include "bsp/log.h"
#include "power/oring.h"
#include "power/share.h"

// Global and used: LTO does not rename or discard it; the host finds its address by the symbol name dbg_cmd
__attribute__((used)) volatile dbg_cmd_t dbg_cmd = {.magic = DBG_CMD_MAGIC};

static uint8_t execute(uint8_t cmd, uint32_t arg)
{
    ch_id_t ch = (ch_id_t)(arg & 1u);
    switch (cmd)
    {
        case DBG_CMD_PATH:
            return (uint8_t)oring_request(ch, (arg & 0x100u) != 0);
        case DBG_CMD_CLEAR_FAULT:
            oring_clear_fault(ch);
            return 0;
        case DBG_CMD_ALL_OFF:
            oring_all_off("remote");
            return 0;
        case DBG_CMD_REPLUG:
            return (uint8_t)oring_replug(ch);
        case DBG_CMD_SHARE:
            if (arg == 0) return share_request(false) ? 0 : 1;
            if (arg > SHARE_MODE_COUNT) return 0xFF;
            share_set_mode((share_mode_t)(arg - 1));
            return share_request(true) ? 0 : 1;     // 1 = preconditions not met, enters automatically once they are
        case DBG_CMD_VOLTAGE:
            return app_set_voltage((uint8_t)((arg >> 16) & 3u), (uint16_t)arg);
        case DBG_CMD_LEARN:
            if (arg == 2) return (uint8_t)share_learn_save();
            if (arg == 1) share_learn_start();
            else share_learn_stop();
            return 0;
        default:
            return 0xFF;
    }
}

void dbg_cmd_process(uint32_t now_ms)
{
    (void)now_ms;
    uint32_t req = dbg_cmd.req;
    if (!req) return;
    dbg_cmd.req = 0;
    if ((req & 0xFFFF0000u) != DBG_REQ_SIG) return;

    uint8_t cmd = (uint8_t)req, seq = (uint8_t)(req >> 8);
    if (cmd == DBG_CMD_SYSTEM_RESET && dbg_cmd.arg == DBG_RESET_KEY)
    {
        LOG("DBG system reset");
        NVIC_SystemReset();
    }
    uint8_t res = execute(cmd, dbg_cmd.arg);
    dbg_cmd.ack = ((uint32_t)seq << 8) | res;
    dbg_cmd.count++;
    LOG("DBG cmd %u arg 0x%lX -> %u", cmd, dbg_cmd.arg, res);
}
