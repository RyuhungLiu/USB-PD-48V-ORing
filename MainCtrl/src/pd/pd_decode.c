#include "pd_decode.h"

pdo_info_t pd_decode_pdo(uint32_t pdo)
{
    pdo_info_t info = {0};

    switch (pdo >> 30)
    {
        case 0:     // Fixed: V[19:10]x50mV, I[9:0]x10mA
            info.kind = PDO_KIND_FIXED;
            info.max_mv = info.min_mv = (uint16_t)(((pdo >> 10) & 0x3FF) * 50);
            info.max_ma = (uint16_t)((pdo & 0x3FF) * 10);
            break;
        case 1:     // Battery: Vmax[29:20] Vmin[19:10]x50mV, P[9:0]x250mW
            info.kind = PDO_KIND_BATTERY;
            info.max_mv = (uint16_t)(((pdo >> 20) & 0x3FF) * 50);
            info.min_mv = (uint16_t)(((pdo >> 10) & 0x3FF) * 50);
            info.pdp_w = (uint16_t)((pdo & 0x3FF) / 4);
            break;
        case 2:     // Variable: Vmax[29:20] Vmin[19:10]x50mV, I[9:0]x10mA
            info.kind = PDO_KIND_VARIABLE;
            info.max_mv = (uint16_t)(((pdo >> 20) & 0x3FF) * 50);
            info.min_mv = (uint16_t)(((pdo >> 10) & 0x3FF) * 50);
            info.max_ma = (uint16_t)((pdo & 0x3FF) * 10);
            break;
        case 3:     // APDO
            switch ((pdo >> 28) & 0x03)
            {
                case 0:     // SPR PPS: Vmax[24:17] Vmin[15:8]x100mV, I[6:0]x50mA
                    info.kind = PDO_KIND_PPS;
                    info.max_mv = (uint16_t)(((pdo >> 17) & 0xFF) * 100);
                    info.min_mv = (uint16_t)(((pdo >> 8) & 0xFF) * 100);
                    info.max_ma = (uint16_t)((pdo & 0x7F) * 50);
                    break;
                case 1:     // EPR AVS: Vmax[25:17] Vmin[15:8]x100mV, PDP[7:0] W
                    info.kind = PDO_KIND_EPR_AVS;
                    info.max_mv = (uint16_t)(((pdo >> 17) & 0x1FF) * 100);
                    info.min_mv = (uint16_t)(((pdo >> 8) & 0xFF) * 100);
                    info.pdp_w = (uint16_t)(pdo & 0xFF);
                    break;
                case 2:     // SPR AVS: 15V PDO I[19:10]x10mA, 20V PDO I[9:0]x10mA
                    info.kind = PDO_KIND_SPR_AVS;
                    info.min_mv = 9000;
                    info.max_mv = 20000;
                    info.max_ma = (uint16_t)((pdo & 0x3FF) * 10);
                    break;
                default:
                    break;
            }
            break;
    }
    return info;
}

bool pd_decode_rdo(uint32_t rdo, const pdo_info_t *pdo, uint16_t *mv, uint16_t *ma)
{
    switch (pdo->kind)
    {
        case PDO_KIND_FIXED:
        case PDO_KIND_VARIABLE:
            *mv = pdo->max_mv;
            *ma = (uint16_t)(((rdo >> 10) & 0x3FF) * 10);     // operating current
            return true;
        case PDO_KIND_PPS:
            *mv = (uint16_t)(((rdo >> 9) & 0xFFF) * 20);
            *ma = (uint16_t)((rdo & 0x7F) * 50);
            return true;
        case PDO_KIND_EPR_AVS:
        case PDO_KIND_SPR_AVS:
            *mv = (uint16_t)(((rdo >> 9) & 0xFFF) * 25);
            *ma = (uint16_t)((rdo & 0x7F) * 50);
            return true;
        default:
            return false;
    }
}

const char *pd_pdo_kind_str(pdo_kind_t kind)
{
    switch (kind)
    {
        case PDO_KIND_FIXED:    return "FIX";
        case PDO_KIND_BATTERY:  return "BAT";
        case PDO_KIND_VARIABLE: return "VAR";
        case PDO_KIND_PPS:      return "PPS";
        case PDO_KIND_EPR_AVS:  return "AVS";
        case PDO_KIND_SPR_AVS:  return "SAVS";
        default:                return "-";
    }
}
