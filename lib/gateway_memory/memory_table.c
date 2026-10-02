/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>

#include <app/gateway/memory_table.h>

void gateway_memory_init(struct gateway_memory *memory)
{
        if (memory == NULL) {
                return;
        }

        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES; i++) {
                memory->entries[i].valid = false;
        }
}

int gateway_memory_update(struct gateway_memory *memory,
                          const struct sensor_reading *reading,
                          uint32_t received_at_ms)
{
        struct gateway_memory_entry *slot = NULL;

        if (memory == NULL || reading == NULL) {
                return -EINVAL;
        }

        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES; i++) {
                struct gateway_memory_entry *candidate = &memory->entries[i];

                if (candidate->valid && candidate->reading.node_id == reading->node_id) {
                        slot = candidate;
                        break;
                }
                if (!candidate->valid && slot == NULL) {
                        slot = candidate;
                }
        }

        if (slot == NULL) {
                return -ENOSPC;
        }

        slot->reading = *reading;
        slot->received_at_ms = received_at_ms;
        slot->valid = true;

        return 0;
}

int gateway_memory_get(struct gateway_memory *memory,
                       uint8_t node_id,
                       struct gateway_memory_entry *entry)
{
        if (memory == NULL || entry == NULL) {
                return -EINVAL;
        }

        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES; i++) {
                if (memory->entries[i].valid && memory->entries[i].reading.node_id == node_id) {
                        *entry = memory->entries[i];
                        return 0;
                }
        }

        return -ENOENT;
}

int gateway_memory_get_all(const struct gateway_memory *memory,
                           struct gateway_memory_entry *entries,
                           size_t capacity)
{
        size_t count = 0;

        if (memory == NULL || entries == NULL) {
                return -EINVAL;
        }

        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES; i++) {
                if (memory->entries[i].valid) {
                        count++;
                }
        }
        if (capacity < count) {
                return -ENOSPC;
        }

        count = 0;
        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES; i++) {
                if (memory->entries[i].valid) {
                        entries[count++] = memory->entries[i];
                }
        }

        return (int)count;
}

int gateway_memory_get_stats(const struct gateway_memory *memory,
                             uint32_t now_ms, uint32_t online_timeout_ms,
                             struct gateway_memory_stats *stats)
{
        struct gateway_memory_stats result = { 0 };

        if (memory == NULL || stats == NULL) {
                return -EINVAL;
        }

        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES; i++) {
                const struct gateway_memory_entry *entry = &memory->entries[i];

                if (!entry->valid) {
                        continue;
                }
                result.valid_nodes++;
                if ((uint32_t)(now_ms - entry->received_at_ms) <= online_timeout_ms) {
                        result.online_nodes++;
                }
        }

        *stats = result;
        return 0;
}
