/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_GATEWAY_SENSORS_JSON_H_
#define APP_GATEWAY_SENSORS_JSON_H_

#include <stddef.h>

#include <zephyr/kernel.h>

#include <app/gateway/memory_table.h>

/* Includes the NUL terminator; fits every entry in a full 16-node table. */
#define GATEWAY_SENSORS_JSON_MAX_SIZE 2048U
/* Tamanho máximo reservado para cada trecho enviado pelo endpoint HTTP. */
#define GATEWAY_SENSORS_JSON_CHUNK_SIZE 128U

/* Estado mantido entre chamadas enquanto o array JSON é gerado em partes. */
struct gateway_sensors_json_stream {
	struct gateway_memory_entry entries[GATEWAY_MEMORY_MAX_NODES];
	size_t count;
	size_t next_entry;
	bool started;
	bool complete;
};

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

/* Copia a tabela sob mutex e permite codificar o snapshot em chamadas sucessivas. */
int gateway_sensors_json_stream_init(const struct gateway_memory *memory,
				     struct k_mutex *memory_lock,
				     struct gateway_sensors_json_stream *stream);
/* Escreve o próximo trecho e informa quando o fechamento do array foi enviado. */
int gateway_sensors_json_encode_next(struct gateway_sensors_json_stream *stream,
				     char *output, size_t capacity,
				     bool *final_chunk);

#endif /* APP_GATEWAY_SENSORS_JSON_H_ */