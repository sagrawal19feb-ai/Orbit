#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Reader for the `assets` partition (xiaozhi pack format):
 *   header (12 B): total_files u32 | checksum u32 | combined_data_length u32
 *   table: total_files x { name[32] | size u32 | offset u32 | width u16 | height u16 }
 *   data:  per file: 0x5A 0x5A marker + raw bytes (offset points at the marker,
 *          relative to the start of the data region)
 * The partition is memory-mapped (zero-copy).
 */

esp_err_t assets_reader_init(void);

/* Returns a pointer to the file contents (inside the mapped partition) or NULL. */
const void *assets_get_file(const char *name, size_t *size);

int assets_get_file_count(void);
