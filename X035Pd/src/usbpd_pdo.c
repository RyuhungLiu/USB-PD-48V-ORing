#include "usbpd_pdo.h"

#define FIXED_MATCH_MV  50      // tolerance for treating a fixed PDO voltage as a match

pd_pdo_t pd_pdo_decode(uint32_t pdo)
{
    pd_pdo_t p = {0};
    if (pdo == 0) return p;

    switch (pdo >> 30)
    {
        case 0:     // Fixed: voltage [19:10] * 50mV, current [9:0] * 10mA
            p.kind = PD_PDO_FIXED;
            p.max_mv = p.min_mv = (uint16_t)(((pdo >> 10) & 0x3FF) * 50);
            p.max_ma = (uint16_t)((pdo & 0x3FF) * 10);
            break;
        case 1:     // Battery: voltage range, power [9:0] * 250mW (not selected)
            p.kind = PD_PDO_BATTERY;
            p.max_mv = (uint16_t)(((pdo >> 20) & 0x3FF) * 50);
            p.min_mv = (uint16_t)(((pdo >> 10) & 0x3FF) * 50);
            break;
        case 2:     // Variable: voltage range * 50mV, current * 10mA
            p.kind = PD_PDO_VARIABLE;
            p.max_mv = (uint16_t)(((pdo >> 20) & 0x3FF) * 50);
            p.min_mv = (uint16_t)(((pdo >> 10) & 0x3FF) * 50);
            p.max_ma = (uint16_t)((pdo & 0x3FF) * 10);
            break;
        default:    // APDO
            switch ((pdo >> 28) & 0x3)
            {
                case 0:     // SPR PPS: Vmax [24:17] / Vmin [15:8] * 100mV, Imax [6:0] * 50mA
                    p.kind = PD_PDO_SPR_PPS;
                    p.max_mv = (uint16_t)(((pdo >> 17) & 0xFF) * 100);
                    p.min_mv = (uint16_t)(((pdo >> 8) & 0xFF) * 100);
                    p.max_ma = (uint16_t)((pdo & 0x7F) * 50);
                    break;
                case 1:     // EPR AVS: Vmax [25:17] / Vmin [15:8] * 100mV, PDP [7:0] W
                    p.kind = PD_PDO_EPR_AVS;
                    p.max_mv = (uint16_t)(((pdo >> 17) & 0x1FF) * 100);
                    p.min_mv = (uint16_t)(((pdo >> 8) & 0xFF) * 100);
                    p.pdp_w = (uint16_t)(pdo & 0xFF);
                    break;
                case 2:     // SPR AVS: 9..15V current [19:10], 15..20V current [9:0] * 10mA
                    p.kind = PD_PDO_SPR_AVS;
                    p.min_mv = 9000;
                    p.max_ma_15v = (uint16_t)(((pdo >> 10) & 0x3FF) * 10);
                    p.max_ma = (uint16_t)((pdo & 0x3FF) * 10);
                    p.max_mv = p.max_ma ? 20000 : 15000;
                    break;
                default:
                    break;
            }
            break;
    }
    return p;
}

uint16_t pd_pdo_max_ma_at(const pd_pdo_t *p, uint16_t mv)
{
    if (p->kind == PD_PDO_NONE || p->kind == PD_PDO_BATTERY) return 0;
    if (p->kind == PD_PDO_FIXED)
    {
        int32_t d = (int32_t)mv - p->max_mv;
        return (d >= -FIXED_MATCH_MV && d <= FIXED_MATCH_MV) ? p->max_ma : 0;
    }
    if (mv < p->min_mv || mv > p->max_mv) return 0;
    switch (p->kind)
    {
        case PD_PDO_SPR_AVS:
            return (mv <= 15000) ? p->max_ma_15v : p->max_ma;
        case PD_PDO_EPR_AVS:
        {
            // min(5A, PDP / V)
            uint32_t i = (uint32_t)p->pdp_w * 1000000u / mv;
            return (uint16_t)(i > 5000 ? 5000 : i);
        }
        default:
            return p->max_ma;
    }
}

static bool kind_adjustable(pd_pdo_kind_t k)
{
    return k == PD_PDO_SPR_PPS || k == PD_PDO_SPR_AVS || k == PD_PDO_EPR_AVS;
}

// Generate the RDO; voltage of adjustable PDOs is aligned to PPS 20mV / AVS 100mV
static void build(uint8_t pos, uint32_t pdo, const pd_pdo_t *p, uint16_t mv, uint16_t ma, pd_select_t *out)
{
    uint16_t lim = pd_pdo_max_ma_at(p, mv);
    out->clamped = false;
    if (ma == 0 || ma > lim)
    {
        out->clamped = ma > lim;
        ma = lim;
    }

    uint32_t rdo = PD_RDO_OBJ_POS(pos) | PD_RDO_NO_USB_SUSPEND | PD_RDO_EPR_MODE_CAPABLE;
    switch (p->kind)
    {
        case PD_PDO_SPR_PPS:
            ma = (uint16_t)(ma / 50 * 50);
            rdo |= ((uint32_t)(mv / 20) & 0xFFF) << 9;
            rdo |= (uint32_t)(ma / 50) & 0x7F;
            break;
        case PD_PDO_SPR_AVS:
        case PD_PDO_EPR_AVS:
            ma = (uint16_t)(ma / 50 * 50);
            rdo |= ((uint32_t)(mv / 25) & 0xFFF) << 9;      // mv is already aligned to 100mV, low 2 bits are 0
            rdo |= (uint32_t)(ma / 50) & 0x7F;
            break;
        default:    // Fixed / Variable: operating current = maximum operating current
            mv = p->max_mv;
            ma = (uint16_t)(ma / 10 * 10);
            rdo |= ((uint32_t)(ma / 10) & 0x3FF) << 10;
            rdo |= (uint32_t)(ma / 10) & 0x3FF;
            break;
    }

    out->pos = pos;
    out->kind = p->kind;
    out->mv = mv;
    out->ma = ma;
    out->rdo = rdo;
    out->pdo = pdo;
}

// Align adjustable PDO voltage and clamp it to the range
static uint16_t align_mv(const pd_pdo_t *p, uint16_t mv, bool *clamped)
{
    uint16_t q = (p->kind == PD_PDO_SPR_PPS) ? 20 : 100;
    uint16_t v = (uint16_t)((mv + q / 2) / q * q);
    if (v < p->min_mv) v = p->min_mv;
    if (v > p->max_mv) v = p->max_mv;
    if (v != mv) *clamped = true;
    return v;
}

// Find the first PDO of the given kind that covers mv (kind = NONE means fixed PDO)
static int8_t find(const uint32_t *caps, uint8_t n, pd_pdo_kind_t kind, uint16_t mv)
{
    for (uint8_t i = 0; i < n; i++)
    {
        pd_pdo_t p = pd_pdo_decode(caps[i]);
        if (p.kind != kind) continue;
        if (kind == PD_PDO_FIXED ? pd_pdo_max_ma_at(&p, mv) != 0 : (mv + 50 >= p.min_mv && mv <= p.max_mv + 50))
            return (int8_t)i;
    }
    return -1;
}

bool pd_select(const uint32_t *caps, uint8_t n, uint8_t mode, uint16_t mv, uint16_t ma, pd_select_t *out)
{
    // Adjustable PDO priority order: PPS -> SPR AVS -> EPR AVS
    static const pd_pdo_kind_t adj_order[] = {PD_PDO_SPR_PPS, PD_PDO_SPR_AVS, PD_PDO_EPR_AVS};
    int8_t idx = -1;

    if (mode == PD_SELECT_FIXED_FIRST) idx = find(caps, n, PD_PDO_FIXED, mv);
    for (uint8_t k = 0; idx < 0 && k < sizeof(adj_order) / sizeof(adj_order[0]); k++)
        idx = find(caps, n, adj_order[k], mv);
    if (idx < 0 && mode == PD_SELECT_ADJ_FIRST) idx = find(caps, n, PD_PDO_FIXED, mv);
    if (idx < 0) idx = find(caps, n, PD_PDO_VARIABLE, mv);
    if (idx < 0) return false;

    pd_pdo_t p = pd_pdo_decode(caps[idx]);
    bool clamped = false;
    uint16_t v = kind_adjustable(p.kind) ? align_mv(&p, mv, &clamped) : mv;
    build((uint8_t)(idx + 1), caps[idx], &p, v, ma, out);
    out->clamped = out->clamped || clamped;
    return true;
}

bool pd_select_nearest(const uint32_t *caps, uint8_t n, uint16_t mv, pd_select_t *out)
{
    // Adjustable PDO covering mv (keep the current voltage unchanged)
    if (pd_select(caps, n, PD_SELECT_ADJ_FIRST, mv, 0, out) && kind_adjustable(out->kind)) return true;

    // Otherwise take the fixed PDO with the closest voltage
    int8_t best = -1;
    int32_t best_d = 0x7FFFFFFF;
    for (uint8_t i = 0; i < n; i++)
    {
        pd_pdo_t p = pd_pdo_decode(caps[i]);
        if (p.kind != PD_PDO_FIXED) continue;
        int32_t d = (int32_t)p.max_mv - mv;
        if (d < 0) d = -d;
        if (d < best_d)
        {
            best_d = d;
            best = (int8_t)i;
        }
    }
    if (best < 0) return false;
    pd_pdo_t p = pd_pdo_decode(caps[best]);
    build((uint8_t)(best + 1), caps[best], &p, p.max_mv, 0, out);
    return true;
}
