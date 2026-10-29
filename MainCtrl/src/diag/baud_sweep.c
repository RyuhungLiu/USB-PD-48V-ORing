// Diagnostic: step-by-step fine tuning of the V203 -> X035 UART baud rate, counting the X035 reply rate to measure the clock deviation between the two sides
// Compiled into the main loop only when DIAG_BAUD_SWEEP=1; only GET_STATUS(0) is sent meanwhile, never any settings command
#include "baud_sweep.h"

#if DIAG_BAUD_SWEEP

#include "drivers/uart_link.h"
#include "pd/pd_proto.h"
#include "bsp/log.h"

#define STEP_MS     8000
#define QUERY_MS    1000

// CH1 = USART2 (72MHz, nominal BRR 78), CH2 = USART1 (144MHz, nominal BRR 156)
// The offset is in units of one CH1 BRR step (about 1.3%); CH2 moves 2 steps in sync to keep the same percentage
static const int8_t steps[] = {0, -1, -2, -3, -4, 1, 2, 3, 4};
#define STEP_COUNT (sizeof(steps) / sizeof(steps[0]))

static uint16_t    brr_nominal[CH_COUNT];
static uint8_t     step;
static uint32_t    step_start, last_query;
static uint16_t    replies[CH_COUNT], queries;
static pd_parser_t parser[CH_COUNT];

static void apply_step(void)
{
    uart_link_set_brr(CH1, (uint16_t)(brr_nominal[CH1] + steps[step]));
    uart_link_set_brr(CH2, (uint16_t)(brr_nominal[CH2] + 2 * steps[step]));
    replies[CH1] = replies[CH2] = 0;
    queries = 0;
}

void baud_sweep_init(uint32_t now_ms)
{
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        brr_nominal[ch] = uart_link_get_brr(ch);
        pd_parser_reset(&parser[ch]);
    }
    step = 0;
    step_start = last_query = now_ms;
    apply_step();
    LOG("SWEEP start: nominal BRR CH1=%u CH2=%u, %u steps x %ums", brr_nominal[CH1], brr_nominal[CH2],
        (unsigned)STEP_COUNT, STEP_MS);
}

void baud_sweep_process(uint32_t now_ms)
{
    uint8_t buf[64];
    pd_frame_t f;

    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        uint16_t n;
        while ((n = uart_link_read(ch, buf, sizeof(buf))) > 0)
        {
            for (uint16_t i = 0; i < n; i++)
            {
                if (pd_parser_feed(&parser[ch], buf[i], now_ms, &f) &&
                    f.type == PD_REPORT_MESSAGE && f.len >= 3 && f.data[0] == PD_CMD_CAT_SYS &&
                    f.data[1] == PD_SYS_GET_STATUS && f.data[2] == 0)
                {
                    replies[ch]++;
                }
            }
        }
    }

    if ((uint32_t)(now_ms - last_query) >= QUERY_MS)
    {
        last_query = now_ms;
        uint8_t pkt[PD_PROTO_CMD_LEN];
        uint8_t len = pd_build_cmd(pkt, PD_CMD_CAT_SYS, PD_SYS_GET_STATUS, 0);
        for (ch_id_t ch = CH1; ch < CH_COUNT; ch++) uart_link_write(ch, pkt, len);
        queries++;
    }

    if ((uint32_t)(now_ms - step_start) >= STEP_MS)
    {
        // Actual baud rate deviation (per mille): nominal BRR / current BRR - 1
        int32_t off1 = (int32_t)(1000L * brr_nominal[CH1] / (brr_nominal[CH1] + steps[step])) - 1000;
        LOG("SWEEP step %d: BRR CH1=%u CH2=%u baud %+ld.%ld%%  sent %u  replies CH1=%u CH2=%u",
            steps[step], brr_nominal[CH1] + steps[step], brr_nominal[CH2] + 2 * steps[step],
            off1 / 10, (off1 < 0 ? -off1 : off1) % 10, queries, replies[CH1], replies[CH2]);
        step = (step + 1) % STEP_COUNT;
        step_start = now_ms;
        apply_step();
    }
}

#endif
