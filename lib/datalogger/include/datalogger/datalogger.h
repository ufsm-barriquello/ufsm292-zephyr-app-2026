/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DATALOGGER_H_
#define DATALOGGER_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DATALOGGER_BUFFER_CAPACITY 16
#define NVS_BOOT_COUNT_ID 1
#define NVS_LAST_ENTRY_ID 2

typedef struct {
    uint32_t timestamp_ms;
    int32_t raw_io_val;
    bool digital_state;
} datalogger_entry_t;

typedef struct {
    datalogger_entry_t entries[DATALOGGER_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
} datalogger_ring_buf_t;

void datalogger_buf_init(datalogger_ring_buf_t *ring);
int datalogger_buf_push(datalogger_ring_buf_t *ring, const datalogger_entry_t *entry);
int datalogger_buf_pop(datalogger_ring_buf_t *ring, datalogger_entry_t *out_entry);
size_t datalogger_buf_count(const datalogger_ring_buf_t *ring);
int datalogger_format_json(const datalogger_entry_t *entry, char *out_buf, size_t buf_len);

/* Funcoes NVS */
int datalogger_nvs_init(struct nvs_fs *fs);
int datalogger_nvs_save_entry(struct nvs_fs *fs, const datalogger_entry_t *entry);
int datalogger_nvs_load_entry(struct nvs_fs *fs, datalogger_entry_t *out_entry);

#ifdef __cplusplus
}
#endif

#endif /* DATALOGGER_H_ */
