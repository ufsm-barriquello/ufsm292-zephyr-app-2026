/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_GATEWAY_SENSORS_RESPONSE_H_
#define APP_GATEWAY_SENSORS_RESPONSE_H_

#include <stddef.h>

#include <app/gateway/sensors_json.h>

/*
 * Encode the Gateway's current sensor readings into a caller-owned buffer.
 * Returns the JSON byte count excluding NUL, or a negative errno.
 * Use GATEWAY_SENSORS_JSON_MAX_SIZE for a complete 16-node response.
 */
int gateway_sensors_json_response(char *output, size_t capacity);

/* Inicia o snapshot consistente usado pela resposta HTTP incremental. */
int gateway_sensors_json_stream_start(struct gateway_sensors_json_stream *stream);

#endif /* APP_GATEWAY_SENSORS_RESPONSE_H_ */