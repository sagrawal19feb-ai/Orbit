#include "assets_reader.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "assets";

/* xiaozhi pack format */
#define PACK_HEADER_SIZE 12
#define PACK_ENTRY_SIZE  44
/* esp-sr srmodels.bin format */
#define SRM_NAME_LEN     32
#define SRM_HEADER_BYTES 4096 /* room to walk any sane model table */

static const esp_partition_t *s_part = NULL;
static const void *s_map = NULL;
static esp_partition_mmap_handle_t s_map_handle = 0;
static size_t s_map_len = 0;
static uint32_t s_file_count = 0;
static bool s_raw_srmodels = false;

static bool name_field_ok(const uint8_t *p, size_t n)
{
    /* non-empty printable ASCII, NUL-padded, nothing after the NUL */
    if (!isprint((int)p[0])) {
        return false;
    }
    bool seen_nul = false;
    for (size_t i = 1; i < n; i++) {
        if (p[i] == 0) {
            seen_nul = true;
        } else if (seen_nul || !isprint((int)p[i])) {
            return false;
        }
    }
    return true;
}

/* srmodels.bin: u32 model_num, then per model: name[32], u32 file_count,
 * per file: name[32], u32 start, u32 len (offsets from the partition base).
 * Walk the table to learn the exact image size. */
static esp_err_t load_raw_srmodels(const uint8_t *probe)
{
    uint32_t model_num;
    memcpy(&model_num, probe, 4);
    size_t off = 4;
    uint32_t total_files = 0;
    uint32_t data_end = 0;
    for (uint32_t m = 0; m < model_num; m++) {
        if (off + SRM_NAME_LEN + 4 > SRM_HEADER_BYTES) {
            return ESP_ERR_INVALID_SIZE;
        }
        char mname[SRM_NAME_LEN + 1] = {0};
        memcpy(mname, probe + off, SRM_NAME_LEN);
        off += SRM_NAME_LEN;
        uint32_t file_count;
        memcpy(&file_count, probe + off, 4);
        off += 4;
        ESP_LOGI(TAG, "model[%u] \"%s\" (%u file(s))", m, mname, file_count);
        for (uint32_t f = 0; f < file_count; f++) {
            if (off + SRM_NAME_LEN + 8 > SRM_HEADER_BYTES) {
                return ESP_ERR_INVALID_SIZE;
            }
            char fname[SRM_NAME_LEN + 1] = {0};
            memcpy(fname, probe + off, SRM_NAME_LEN);
            off += SRM_NAME_LEN;
            uint32_t start, len;
            memcpy(&start, probe + off, 4);
            memcpy(&len, probe + off + 4, 4);
            off += 8;
            if ((uint64_t)start + len > data_end) {
                data_end = start + len;
            }
            ESP_LOGI(TAG, "  %-16s start=0x%05x len=%u", fname, start, len);
        }
        total_files += file_count;
    }
    size_t image_len = off + data_end;
    if (image_len > s_part->size) {
        ESP_LOGE(TAG, "srmodels image (%u) bigger than partition (%u)",
                 (unsigned)image_len, (unsigned)s_part->size);
        return ESP_ERR_INVALID_SIZE;
    }
    s_map_len = image_len;
    s_file_count = total_files;
    s_raw_srmodels = true;
    return ESP_OK;
}

esp_err_t assets_reader_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (!s_part) {
        ESP_LOGE(TAG, "assets partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    static uint8_t probe[SRM_HEADER_BYTES];
    ESP_ERROR_CHECK(esp_partition_read(s_part, 0, probe, sizeof(probe)));

    /* auto-detect the format: raw esp-sr srmodels.bin (what Orbit flashes at
     * 0x420000) vs the xiaozhi pack format (12-byte header + 44-byte entries
     * + 0x5A5A markers) */
    uint32_t n_models, n_files0;
    memcpy(&n_models, probe, 4);
    bool srmodels_ok = (n_models >= 1 && n_models <= 16) &&
                       name_field_ok(probe + 4, SRM_NAME_LEN);
    if (srmodels_ok) {
        memcpy(&n_files0, probe + 4 + SRM_NAME_LEN, 4);
        srmodels_ok = (n_files0 >= 1 && n_files0 <= 64) &&
                      name_field_ok(probe + 8 + SRM_NAME_LEN, SRM_NAME_LEN);
    }
    uint32_t pack_files, pack_data_len;
    memcpy(&pack_files, probe, 4);
    memcpy(&pack_data_len, probe + 8, 4);
    bool pack_ok = (pack_files >= 1 && pack_files <= 64) &&
                   (pack_data_len >= pack_files * PACK_ENTRY_SIZE) &&
                   ((uint64_t)PACK_HEADER_SIZE + pack_data_len <= s_part->size) &&
                   name_field_ok(probe + PACK_HEADER_SIZE, SRM_NAME_LEN);

    esp_err_t err;
    if (srmodels_ok) {
        ESP_LOGI(TAG, "assets: raw esp-sr srmodels.bin detected");
        err = load_raw_srmodels(probe);
    } else if (pack_ok) {
        ESP_LOGI(TAG, "assets: xiaozhi pack format detected (%u file(s))", pack_files);
        s_map_len = PACK_HEADER_SIZE + pack_data_len;
        s_file_count = pack_files;
        err = ESP_OK;
    } else {
        ESP_LOGE(TAG, "assets: unrecognized format (not srmodels.bin, not a pack)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = esp_partition_mmap(s_part, 0, s_map_len,
                             ESP_PARTITION_MMAP_DATA, &s_map, &s_map_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "assets: mapped %u bytes (%u file(s))",
             (unsigned)s_map_len, s_file_count);
    return ESP_OK;
}

const void *assets_get_file(const char *name, size_t *size)
{
    if (!s_map || !name) {
        return NULL;
    }
    if (s_raw_srmodels) {
        /* the partition holds srmodels.bin itself */
        if (strcmp(name, "srmodels.bin") != 0) {
            return NULL;
        }
        if (size) {
            *size = s_map_len;
        }
        return s_map;
    }
    /* xiaozhi pack format */
    size_t table_off = PACK_HEADER_SIZE;
    size_t data_off = table_off + s_file_count * PACK_ENTRY_SIZE;
    for (uint32_t i = 0; i < s_file_count; i++) {
        const uint8_t *e = (const uint8_t *)s_map + table_off + i * PACK_ENTRY_SIZE;
        char fname[SRM_NAME_LEN + 1] = {0};
        memcpy(fname, e, SRM_NAME_LEN);
        if (strcmp(fname, name) != 0) {
            continue;
        }
        uint32_t fsize, off;
        memcpy(&fsize, e + 32, 4);
        memcpy(&off, e + 36, 4);
        const uint8_t *marker = (const uint8_t *)s_map + data_off + off;
        if (marker[0] != 0x5A || marker[1] != 0x5A) {
            ESP_LOGE(TAG, "bad 0x5A5A marker for %s", name);
            return NULL;
        }
        if (size) {
            *size = fsize;
        }
        return marker + 2;
    }
    return NULL;
}

int assets_get_file_count(void)
{
    return (int)s_file_count;
}
