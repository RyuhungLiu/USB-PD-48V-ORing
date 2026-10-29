#pragma once

#include <stdbool.h>
#include <stdint.h>

// On-chip flash erase/write (for parameter storage)
//   - Erase unit 256 bytes (page erase in fast programming mode), write unit half-word (standard mode), word-by-word verify after write
//   - CPU instruction fetch stalls during erase/write; the main loop does not run meanwhile

#define FLASH_PAGE_SIZE     256u

// addr must be 256-byte aligned within the 0x08000000 mapped area; after erase the whole page is verified to be uniform
bool flash_erase_page(uint32_t addr);

// addr and len must be 4-byte aligned; read back and compare after write, returns false on mismatch
bool flash_write(uint32_t addr, const void *data, uint32_t len);
