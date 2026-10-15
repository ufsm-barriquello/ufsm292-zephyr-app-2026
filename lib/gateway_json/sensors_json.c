/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/printk.h>

#include <app/gateway/sensors_json.h>

static int append_entry(char *output, size_t capacity, size_t *offset,
			const struct gateway_memory_entry *entry, bool first)
{
	const struct sensor_reading *reading = &entry->reading;
	int32_t temp = reading->temp_c_x100;
	uint32_t temp_magnitude = temp < 0 ? (uint32_t)-temp : (uint32_t)temp;
	int written;

	written = snprintk(output + *offset, capacity - *offset,
			   "%s{\"node_id\":%u,\"light\":%u,\"temp_c\":%s%u.%02u,"
			   "\"accel\":[%d,%d,%d],\"last_seen_ms\":%u}",
			   first ? "" : ",", (unsigned int)reading->node_id,
			   (unsigned int)reading->light, temp < 0 ? "-" : "",
			   (unsigned int)(temp_magnitude / 100U),
			   (unsigned int)(temp_magnitude % 100U),
			   (int)reading->accel[0], (int)reading->accel[1],
			   (int)reading->accel[2], (unsigned int)entry->received_at_ms);
	if (written < 0 || (size_t)written >= capacity - *offset) {
		return -ENOSPC;
	}

	*offset += (size_t)written;
	return 0;
}

int gateway_sensors_json_encode(const struct gateway_memory *memory,
				struct k_mutex *memory_lock,
				char *output, size_t capacity)
{
	struct gateway_memory_entry snapshot[GATEWAY_MEMORY_MAX_NODES];
	int count;
	size_t offset = 0;
	int ret;

	if (memory == NULL || memory_lock == NULL || output == NULL) {
		return -EINVAL;
	}
	if (capacity == 0U) {
		return -ENOSPC;
	}

	output[0] = '\0';
	k_mutex_lock(memory_lock, K_FOREVER);
	count = gateway_memory_get_all(memory, snapshot,
				       GATEWAY_MEMORY_MAX_NODES);
	k_mutex_unlock(memory_lock);
	if (count < 0) {
		return count;
	}

	if (capacity < 3U) {
		return -ENOSPC;
	}
	output[offset++] = '[';
	output[offset] = '\0';
	for (int i = 0; i < count; i++) {
		ret = append_entry(output, capacity, &offset, &snapshot[i], i == 0);
		if (ret < 0) {
			output[0] = '\0';
			return ret;
		}
	}

	if (capacity - offset < 2U) {
		output[0] = '\0';
		return -ENOSPC;
	}
	output[offset++] = ']';
	output[offset] = '\0';

	return (int)offset;
}

int gateway_sensors_json_stream_init(const struct gateway_memory *memory,
				     struct k_mutex *memory_lock,
				     struct gateway_sensors_json_stream *stream)
{
	int count;

	if (memory == NULL || memory_lock == NULL || stream == NULL) {
		return -EINVAL;
	}

	memset(stream, 0, sizeof(*stream));
	/* Mantém a cópia consistente e libera o mutex antes da codificação/envio. */
	k_mutex_lock(memory_lock, K_FOREVER);
	count = gateway_memory_get_all(memory, stream->entries,
				       GATEWAY_MEMORY_MAX_NODES);
	k_mutex_unlock(memory_lock);
	if (count < 0) {
		return count;
	}

	stream->count = (size_t)count;
	return 0;
}

int gateway_sensors_json_encode_next(struct gateway_sensors_json_stream *stream,
				     char *output, size_t capacity,
				     bool *final_chunk)
{
	size_t offset = 0;
	int ret;

	if (stream == NULL || output == NULL || final_chunk == NULL) {
		return -EINVAL;
	}
	if (capacity < GATEWAY_SENSORS_JSON_CHUNK_SIZE) {
		return -ENOSPC;
	}
	if (stream->complete) {
		return -EALREADY;
	}

	output[0] = '\0';
	if (!stream->started) {
		output[offset++] = '[';
		stream->started = true;
	}

	/* Só avança a entrada quando ela couber inteira no trecho atual. */
	while (stream->next_entry < stream->count) {
		char entry_json[GATEWAY_SENSORS_JSON_CHUNK_SIZE];
		size_t entry_len = 0;

		ret = append_entry(entry_json, sizeof(entry_json), &entry_len,
				   &stream->entries[stream->next_entry],
				   stream->next_entry == 0U);
		if (ret < 0) {
			return ret;
		}

		if (capacity - offset < entry_len + 2U) {
			if (offset == 1U && stream->next_entry == 0U) {
				return -ENOSPC;
			}
			break;
		}

		memcpy(output + offset, entry_json, entry_len);
		offset += entry_len;
		stream->next_entry++;
	}

	if (stream->next_entry == stream->count) {
		if (capacity - offset < 2U) {
			return -ENOSPC;
		}
		output[offset++] = ']';
		stream->complete = true;
		*final_chunk = true;
	} else {
		*final_chunk = false;
	}

	output[offset] = '\0';
	return (int)offset;
}