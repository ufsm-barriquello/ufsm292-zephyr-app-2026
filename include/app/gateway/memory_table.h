/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_GATEWAY_MEMORY_TABLE_H_
#define APP_GATEWAY_MEMORY_TABLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <app/lib/sensor_frame.h>

/* Number of stored nodes, not a limit on the uint8_t node ID range. */
#define GATEWAY_MEMORY_MAX_NODES 16

struct gateway_memory_entry {
        bool valid;
        struct sensor_reading reading;
        uint32_t received_at_ms;
};

struct gateway_memory {
        struct gateway_memory_entry entries[GATEWAY_MEMORY_MAX_NODES];
};

struct gateway_memory_stats {
        size_t valid_nodes;
        size_t online_nodes;
};

/* Serialize all table access when sharing it between threads. */
void gateway_memory_init(struct gateway_memory *memory);

/* Returns -ENOSPC for a new node when full; existing nodes still update. */
int gateway_memory_update(struct gateway_memory *memory,
                          const struct sensor_reading *reading,
                          uint32_t received_at_ms);

int gateway_memory_get(struct gateway_memory *memory,
                       uint8_t node_id,
                       struct gateway_memory_entry *entry);

/*
 * Copy valid entries in slot order; returns their count or a negative errno.
 * An undersized output returns -ENOSPC without writing a partial snapshot.
 * entries must not overlap the table. No allocation or JSON encoding is done.
 */
int gateway_memory_get_all(const struct gateway_memory *memory,
                           struct gateway_memory_entry *entries,
                           size_t capacity);

/*
 * Count valid entries and nodes with reception age <= online_timeout_ms.
 * Use gateway uptime (the same clock as received_at_ms), not sensor uptime.
 * Unsigned elapsed time handles wrap provided age is less than 2^32 ms.
 * Stale entries remain valid and are included in get_all().
 */
int gateway_memory_get_stats(const struct gateway_memory *memory,
                             uint32_t now_ms, uint32_t online_timeout_ms,
                             struct gateway_memory_stats *stats);

#endif
