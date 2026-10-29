#include "key.h"

#include "board.h"

#define EVT_QUEUE_SIZE 8

typedef enum key_state_t
{
    ST_IDLE,
    ST_PRESSED,         // first press in progress
    ST_WAIT_SECOND,     // released, waiting for a possible second press
    ST_PRESSED_SECOND,  // second press in progress
    ST_WAIT_THIRD,      // double click done, waiting for a possible third press
    ST_PRESSED_THIRD,   // third press in progress
    ST_WAIT_RELEASE,    // long press already fired, waiting for release
} key_state_t;

static key_state_t state;
static bool        multi_tap = true;
static uint32_t    t_mark;          // time of press or release

// Debounce
static bool        raw_last;
static bool        stable;
static uint32_t    raw_change_ms;

static key_evt_t   queue[EVT_QUEUE_SIZE];
static uint8_t     q_head, q_tail;

static void push(key_evt_t evt)
{
    uint8_t next = (q_head + 1) % EVT_QUEUE_SIZE;
    if (next == q_tail) return;     // drop the new event when full
    queue[q_head] = evt;
    q_head = next;
}

static bool read_raw(void)
{
    bool level = GPIO_ReadInputDataBit(KEY_GPIO_PORT, KEY_GPIO_PIN) == Bit_SET;
    return KEY_ACTIVE_HIGH ? level : !level;
}

void key_init(void)
{
    RCC_APB2PeriphClockCmd(KEY_GPIO_CLK, ENABLE);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = KEY_GPIO_PIN;
    gpio.GPIO_Mode = KEY_ACTIVE_HIGH ? GPIO_Mode_IPD : GPIO_Mode_IPU;   // stays "not pressed" when the module is not connected
    GPIO_Init(KEY_GPIO_PORT, &gpio);

    state = ST_IDLE;
    raw_last = stable = false;
    q_head = q_tail = 0;
}

void key_set_multi_tap(bool on)
{
    multi_tap = on;
}

void key_scan(uint32_t now_ms)
{
    bool raw = read_raw();
    if (raw != raw_last)
    {
        raw_last = raw;
        raw_change_ms = now_ms;
    }
    if ((uint32_t)(now_ms - raw_change_ms) >= KEY_DEBOUNCE_MS) stable = raw;

    bool down = stable;
    uint32_t dt = now_ms - t_mark;

    switch (state)
    {
        case ST_IDLE:
            if (down) { state = ST_PRESSED; t_mark = now_ms; }
            break;

        case ST_PRESSED:
            if (!down && !multi_tap) { push(KEY_EVT_CLICK); state = ST_IDLE; }
            else if (!down) { state = ST_WAIT_SECOND; t_mark = now_ms; }
            else if (dt >= KEY_LONG_MS) { push(KEY_EVT_LONG); state = ST_WAIT_RELEASE; }
            break;

        case ST_WAIT_SECOND:
            if (down) { state = ST_PRESSED_SECOND; t_mark = now_ms; }
            else if (dt >= KEY_DOUBLE_MS) { push(KEY_EVT_CLICK); state = ST_IDLE; }
            break;

        case ST_PRESSED_SECOND:
            if (!down) { push(KEY_EVT_DOUBLE); state = ST_WAIT_THIRD; t_mark = now_ms; }
            else if (dt >= KEY_LONG_MS) { push(KEY_EVT_LONG); state = ST_WAIT_RELEASE; }
            break;

        case ST_WAIT_THIRD:
            if (down) { state = ST_PRESSED_THIRD; t_mark = now_ms; }
            else if (dt >= KEY_DOUBLE_MS) state = ST_IDLE;
            break;

        case ST_PRESSED_THIRD:
            if (!down) { push(KEY_EVT_TRIPLE); state = ST_IDLE; }
            else if (dt >= KEY_LONG_MS) state = ST_WAIT_RELEASE;
            break;

        case ST_WAIT_RELEASE:
            if (!down) state = ST_IDLE;
            break;
    }
}

key_evt_t key_get_event(void)
{
    if (q_tail == q_head) return KEY_EVT_NONE;
    key_evt_t evt = queue[q_tail];
    q_tail = (q_tail + 1) % EVT_QUEUE_SIZE;
    return evt;
}

const char *key_evt_str(key_evt_t evt)
{
    switch (evt)
    {
        case KEY_EVT_CLICK:  return "CLICK";
        case KEY_EVT_DOUBLE: return "DOUBLE";
        case KEY_EVT_TRIPLE: return "TRIPLE";
        case KEY_EVT_LONG:   return "LONG";
        default:             return "NONE";
    }
}
