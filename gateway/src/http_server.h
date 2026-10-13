/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAY_HTTP_SERVER_H_
#define GATEWAY_HTTP_SERVER_H_

#include <stddef.h>

/* Runs the single-client HTTP loop on the calling thread until a socket error. */
int gateway_http_run(void);
int gateway_health_json_response(char *output, size_t capacity);

#endif
