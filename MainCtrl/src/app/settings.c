#include "settings.h"

#include <stddef.h>
#include <string.h>
#include "bsp/flash.h"
#include "bsp/log.h"
#include "bsp/delay.h"
#include "bsp/resetlog.h"
#include "power/meter.h"
#include "app/stats.h"

#define SETTINGS_SAVE_DELAY_MS      2000                // wait after a settings change, to merge consecutive edits
#define SETTINGS_ENERGY_PERIOD_MS   (10u * 60u * 1000u) // periodic save of accumulated energy

// Last 4KB of flash; platformio.ini uses board_upload.maximum_size to guarantee the program never enters this area.
// Rotates using 256-byte fast page erase (2 slots per page, 32 slots total, each page erased once per 32 saves); a page is erased only when entering it,
// so the previous valid record is always on the previous page and a power loss during erase/write never loses both the new and the old record
#define STORE_BASE      0x0800F000u
#define STORE_SIZE      4096u
#define SLOT_SIZE       128u
#define SLOTS_PER_PAGE  (FLASH_PAGE_SIZE / SLOT_SIZE)
#define SLOT_COUNT      (STORE_SIZE / SLOT_SIZE)
// Storage area before v0.0.85: last 8KB, 4KB sector erase, SET1 records. Read only when no SET2 is found (beyond the first 4KB it will be occupied by the program)
#define OLD_BASE        0x0800E000u
#define OLD_SIZE        8192u

// Change the magic when the structure changes; old records then become invalid and defaults are restored
#define RECORD_MAGIC    0x32544553u     // "SET2": 128-byte slot (v0.0.86 added the MANUAL learning result)
// Old format "SET1": 64-byte slot, first 60 bytes identical to SET2, followed by CRC. Read and reused when no SET2 is found (upgrade keeps settings and totals)
#define RECORD1_MAGIC   0x31544553u
#define RECORD1_SIZE    64u

typedef struct record_t
{
    uint32_t magic;
    uint32_t seq;
    uint8_t  sync_on;
    uint8_t  auto_on;
    uint8_t  balance_on;                // formerly a reserved byte: 0 = off in old records, format and magic unchanged
    uint8_t  balance_mode;              // formerly a reserved byte: bit0..3 current sharing mode, bit4..5 OCP level, bit6 = Auto EPR off; 0 in old records
    uint16_t target_mv[CH_COUNT];
    uint64_t energy_mwms[CH_COUNT];
    uint64_t charge_mams[CH_COUNT];
    uint32_t life_boots;                // the following three use the formerly reserved 12 bytes; 0 in old records
    uint32_t life_on_s;
    uint32_t life_trips;
    share_profile_t profile;            // the following are new in SET2
    uint8_t  reserved[44];
    uint32_t crc;                       // placed last: power loss during write leaves a CRC mismatch
} record_t;

_Static_assert(sizeof(record_t) == SLOT_SIZE, "record must fill one slot");

static settings_t cur;
static record_t   saved;                // most recently written (or read) record
static int32_t    slot_idx = -1;        // slot of the most recent record, -1 = none
static bool       dirty;
static uint32_t   dirty_ms;
static uint32_t   last_save_ms;
static uint32_t   last_energy_ms;
static bool       write_error;
static bool       loaded;

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--)
    {
        crc ^= *p++;
        for (uint8_t k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

static uint32_t slot_addr(uint32_t idx)
{
    return STORE_BASE + idx * SLOT_SIZE;
}

static bool record_valid(const record_t *r)
{
    return r->magic == RECORD_MAGIC && r->crc == crc32(r, offsetof(record_t, crc));
}

static void defaults(void)
{
    cur = (settings_t){.sync_on = true, .auto_on = false, .auto_epr = true};
}

void settings_init(void)
{
    defaults();

    // Find the valid record with the highest sequence number (comparison handles wraparound)
    record_t r;
    for (uint32_t i = 0; i < SLOT_COUNT; i++)
    {
        memcpy(&r, (const void *)slot_addr(i), sizeof(r));
        if (!record_valid(&r)) continue;
        if (slot_idx < 0 || (int32_t)(r.seq - saved.seq) > 0)
        {
            saved = r;
            slot_idx = (int32_t)i;
        }
    }

    // Old format: take the SET1 record with the highest sequence number. slot_idx stays -1, so the first save erases/writes from the start as SET2
    bool old = false;
    for (uint32_t i = 0; slot_idx < 0 && i < OLD_SIZE / RECORD1_SIZE; i++)
    {
        memset(&r, 0, sizeof(r));
        memcpy(&r, (const void *)(OLD_BASE + i * RECORD1_SIZE), RECORD1_SIZE - 4);
        uint32_t crc1 = *(const uint32_t *)(OLD_BASE + i * RECORD1_SIZE + RECORD1_SIZE - 4);
        if (r.magic != RECORD1_MAGIC || crc1 != crc32(&r, RECORD1_SIZE - 4)) continue;
        if (!old || (int32_t)(r.seq - saved.seq) > 0) saved = r;
        old = true;
    }

    if (slot_idx < 0 && !old)
    {
        LOG("SETTINGS none stored, defaults");
        return;
    }

    loaded = true;
    cur.sync_on = saved.sync_on != 0;
    cur.auto_on = saved.auto_on != 0;
    cur.balance_on = saved.balance_on != 0;
    cur.balance_mode = saved.balance_mode & 0x0F;
    cur.ocp_level = (saved.balance_mode >> 4) & 0x03;
    cur.auto_epr = !(saved.balance_mode & 0x40);
    cur.profile = saved.profile;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        cur.target_mv[ch] = saved.target_mv[ch];
        meter_restore_energy(ch, saved.energy_mwms[ch], saved.charge_mams[ch]);
    }
    LOG("SETTINGS #%lu slot %ld: sync=%u auto=%u balance=%u target=%u/%umV", saved.seq, slot_idx,
        cur.sync_on, cur.auto_on, cur.balance_on, cur.target_mv[CH1], cur.target_mv[CH2]);
    if (old)
    {
        // Old format: on delayed save it is written as SET2 into the new storage area (the old area may later be occupied by the program)
        dirty = true;
        dirty_ms = millis();
    }
}

static bool save(uint32_t now_ms)
{
    resetlog_mark(RL_STAGE_FLASH, now_ms);

    record_t r = {0};
    r.magic = RECORD_MAGIC;
    r.seq = (slot_idx < 0) ? 1 : saved.seq + 1;
    r.sync_on = cur.sync_on;
    r.auto_on = cur.auto_on;
    r.balance_on = cur.balance_on;
    r.balance_mode = (uint8_t)((cur.balance_mode & 0x0F) | (cur.ocp_level << 4) | (cur.auto_epr ? 0 : 0x40));
    // Accumulated statistics: if stats is not initialized yet (boot count 0), keep the values read, to avoid overwriting with 0
    bool stats_ready = stats_life_boots() != 0;
    r.life_boots = stats_ready ? stats_life_boots() : saved.life_boots;
    r.life_on_s  = stats_ready ? stats_life_on_s()  : saved.life_on_s;
    r.life_trips = stats_ready ? stats_life_trips() : saved.life_trips;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        const meter_ch_t *m = meter_get(ch);
        r.target_mv[ch] = cur.target_mv[ch];
        r.energy_mwms[ch] = m->energy_mwms;
        r.charge_mams[ch] = m->charge_mams;
    }
    r.profile = cur.profile;
    r.crc = crc32(&r, offsetof(record_t, crc));

    // Start from the slot after the most recent record; erase first when entering a new page. If write verification fails (slot not blank), try the next slot
    uint32_t idx = (slot_idx < 0) ? 0 : (uint32_t)(slot_idx + 1) % SLOT_COUNT;
    bool need_erase = (slot_idx < 0);
    uint32_t start_ms = millis();
    (void)start_ms;     // used only for logging

    for (uint32_t tries = 0; tries < SLOT_COUNT; tries++)
    {
        if (need_erase || idx % SLOTS_PER_PAGE == 0)
        {
            uint32_t page = STORE_BASE + (idx / SLOTS_PER_PAGE) * FLASH_PAGE_SIZE;
            idx = (idx / SLOTS_PER_PAGE) * SLOTS_PER_PAGE;
            need_erase = false;
            if (!flash_erase_page(page))
            {
                LOG("SETTINGS erase 0x%08lX failed", page);
                break;
            }
        }

        if (flash_write(slot_addr(idx), &r, sizeof(r)))
        {
            saved = r;
            slot_idx = (int32_t)idx;
            last_save_ms = now_ms ? now_ms : 1;
            write_error = false;
            LOG("SETTINGS saved #%lu slot %lu (%lums)", r.seq, idx, millis() - start_ms);
            return true;
        }
        idx = (idx + 1) % SLOT_COUNT;
    }

    write_error = true;
    LOG("SETTINGS save failed");
    return false;
}

static bool energy_changed(void)
{
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        if (meter_get(ch)->energy_mwms != saved.energy_mwms[ch]) return true;
    }
    return false;
}

void settings_process(uint32_t now_ms)
{
    if (dirty && (uint32_t)(now_ms - dirty_ms) >= SETTINGS_SAVE_DELAY_MS)
    {
        dirty = false;
        last_energy_ms = now_ms;
        save(now_ms);
        return;
    }

    if ((uint32_t)(now_ms - last_energy_ms) >= SETTINGS_ENERGY_PERIOD_MS)
    {
        last_energy_ms = now_ms;
        if (energy_changed()) save(now_ms);
    }
}

static void mark_dirty(void)
{
    dirty = true;
    dirty_ms = millis();
}

const settings_t *settings_get(void)
{
    return &cur;
}

void settings_set_sync(bool on)
{
    if (cur.sync_on == on) return;
    cur.sync_on = on;
    mark_dirty();
}

void settings_set_auto_on(bool on)
{
    if (cur.auto_on == on) return;
    cur.auto_on = on;
    mark_dirty();
}

void settings_set_balance(bool on)
{
    if (cur.balance_on == on) return;
    cur.balance_on = on;
    mark_dirty();
}

void settings_set_balance_mode(uint8_t mode)
{
    if (cur.balance_mode == mode) return;
    cur.balance_mode = mode;
    mark_dirty();
}

void settings_set_ocp_level(uint8_t level)
{
    level &= 0x03;
    if (cur.ocp_level == level) return;
    cur.ocp_level = level;
    mark_dirty();
}

void settings_set_auto_epr(bool on)
{
    if (cur.auto_epr == on) return;
    cur.auto_epr = on;
    mark_dirty();
}

void settings_set_target(ch_id_t ch, uint16_t mv)
{
    if (ch >= CH_COUNT || cur.target_mv[ch] == mv) return;
    cur.target_mv[ch] = mv;
    mark_dirty();
}

void settings_set_profile(const share_profile_t *p)
{
    cur.profile = *p;
    mark_dirty();
    settings_save_now();
}

void settings_get_lifetime(uint32_t *boots, uint32_t *on_s, uint32_t *trips)
{
    *boots = saved.life_boots;
    *on_s = saved.life_on_s;
    *trips = saved.life_trips;
}

void settings_request_save(void)
{
    mark_dirty();
}

void settings_save_now(void)
{
    dirty = false;
    last_energy_ms = millis();
    save(millis());
}

void settings_reset_energy(void)
{
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++) meter_restore_energy(ch, 0, 0);
    settings_save_now();
}

uint32_t settings_seq(void)
{
    return (slot_idx < 0) ? 0 : saved.seq;
}

uint32_t settings_last_save_ms(void)
{
    return last_save_ms;
}

bool settings_error(void)
{
    return write_error;
}

bool settings_loaded(void)
{
    return loaded;
}
