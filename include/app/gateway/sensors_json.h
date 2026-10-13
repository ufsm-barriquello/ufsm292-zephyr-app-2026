/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_GATEWAY_SENSORS_JSON_H_
#define APP_GATEWAY_SENSORS_JSON_H_

#include <stddef.h>

#include <zephyr/kernel.h>

#include <app/gateway/memory_table.h>

/* Includes the NUL terminator; conservative bound for every 4-node response. */
#define GATEWAY_SENSORS_JSON_MAX_SIZE (3U + 128U * GATEWAY_MEMORY_MAX_NODES)

/*
 * Encode a consistent snapshot of the table as a JSON array. The caller
 * supplies the mutex used by every table writer and a NUL-terminated output
 * buffer. Values are encoded without heap allocation; temp_c_x100 is emitted
 * in Celsius and accel[] retains the signed sensor values stored in the frame.
 * last_seen_ms is the Gateway uptime at the last reception.
 *
 * Returns the JSON byte count excluding the NUL terminator, or a negative
 * errno. On -ENOSPC, output is an empty string when capacity is nonzero.
 */
int gateway_sensors_json_encode(const struct gateway_memory *memory,
				struct k_mutex *memory_lock,
				char *output, size_t capacity);

#endif /* APP_GATEWAY_SENSORS_JSON_H_ */