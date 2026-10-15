/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/sys/reboot.h>

#include <app/gateway/memory_table.h>
#include <app/gateway/sensors_response.h>

#include "gateway_state.h"
#include "http_server.h"

LOG_MODULE_REGISTER(gateway_http, CONFIG_GATEWAY_LOG_LEVEL);

static uint16_t gateway_http_port = 80;
/* O HTTP/1 chama o handler novamente até final_chunk; o limite é um cliente. */
static struct gateway_sensors_json_stream gateway_sensors_stream;
static char gateway_sensors_http_chunk[GATEWAY_SENSORS_JSON_CHUNK_SIZE];
static bool gateway_sensors_stream_active;

static void gateway_reset_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(gateway_reset_work, gateway_reset_work_handler);

static void gateway_http_set_response(struct http_response_ctx *response_ctx,
				      enum http_status status,
				      const char *body, size_t body_len)
{
	response_ctx->status = status;
	response_ctx->body = (const uint8_t *)body;
	response_ctx->body_len = body_len;
	response_ctx->final_chunk = true;
}

static int gateway_sensors_http_handler(struct http_client_ctx *client,
					enum http_transaction_status status,
					const struct http_request_ctx *request_ctx,
					struct http_response_ctx *response_ctx,
					void *user_data)
{
	static const char error_body[] = "{\"error\":\"could not encode sensor data\"}";
	bool final_chunk;
	int ret;

	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_TRANSACTION_COMPLETE ||
	    status == HTTP_SERVER_TRANSACTION_ABORTED) {
		/* Descarta o estado, seja após concluir a resposta ou abortar a conexão. */
		gateway_sensors_stream_active = false;
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (!gateway_sensors_stream_active) {
		/* Todas as partes da resposta usam a mesma cópia da tabela. */
		ret = gateway_sensors_json_stream_start(&gateway_sensors_stream);
		if (ret < 0) {
			LOG_ERR("Could not snapshot sensor data (%d)", ret);
			gateway_http_set_response(response_ctx, HTTP_500_INTERNAL_SERVER_ERROR,
					  error_body, sizeof(error_body) - 1U);
			return 0;
		}
		gateway_sensors_stream_active = true;
	}

	ret = gateway_sensors_json_encode_next(&gateway_sensors_stream,
					       gateway_sensors_http_chunk,
					       sizeof(gateway_sensors_http_chunk),
					       &final_chunk);
	if (ret < 0) {
		LOG_ERR("Could not encode sensor data chunk (%d)", ret);
		gateway_sensors_stream_active = false;
		return ret;
	}

	gateway_http_set_response(response_ctx, HTTP_200_OK,
				  gateway_sensors_http_chunk, (size_t)ret);
	response_ctx->final_chunk = final_chunk;
	return 0;
}

static struct http_resource_detail_dynamic gateway_sensors_resource = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "application/json",
	},
	.cb = gateway_sensors_http_handler,
};

static int gateway_health_http_handler(struct http_client_ctx *client,
				       enum http_transaction_status status,
				       const struct http_request_ctx *request_ctx,
				       struct http_response_ctx *response_ctx,
				       void *user_data)
{
	static char body[80];
	struct gateway_memory_stats stats;
	uint32_t uptime_ms;
	int ret;

	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	ret = gateway_get_health_stats(&stats);
	if (ret < 0) {
		LOG_ERR("Could not read gateway health statistics (%d)", ret);
		return ret;
	}

	uptime_ms = k_uptime_get_32();
	ret = snprintk(body, sizeof(body),
		       "status=running\nuptime_ms=%u\nonline_nodes=%u\n",
		       (unsigned int)uptime_ms, (unsigned int)stats.online_nodes);
	if (ret < 0 || (size_t)ret >= sizeof(body)) {
		return -ENOMEM;
	}

	gateway_http_set_response(response_ctx, HTTP_200_OK, body, (size_t)ret);
	return 0;
}

static struct http_resource_detail_dynamic gateway_health_resource = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "text/plain",
	},
	.cb = gateway_health_http_handler,
};

static int gateway_reset_http_handler(struct http_client_ctx *client,
				      enum http_transaction_status status,
				      const struct http_request_ctx *request_ctx,
				      struct http_response_ctx *response_ctx,
				      void *user_data)
{
	static const char body[] = "Gateway restart scheduled.\n";
	int ret;

	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	ret = k_work_schedule(&gateway_reset_work, K_MSEC(500));
	if (ret < 0) {
		LOG_ERR("Could not schedule gateway restart (%d)", ret);
		return ret;
	}

	gateway_http_set_response(response_ctx, HTTP_202_ACCEPTED,
				  body, sizeof(body) - 1U);
	return 0;
}

static struct http_resource_detail_dynamic gateway_reset_resource = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		.content_type = "text/plain",
	},
	.cb = gateway_reset_http_handler,
};

HTTP_SERVICE_DEFINE(gateway_http_service, NULL, &gateway_http_port, 1, 2,
		    NULL, NULL, NULL);

HTTP_RESOURCE_DEFINE(gateway_sensors_http_resource, gateway_http_service,
		     "/sensors", &gateway_sensors_resource);
HTTP_RESOURCE_DEFINE(gateway_health_http_resource, gateway_http_service,
		     "/health", &gateway_health_resource);
HTTP_RESOURCE_DEFINE(gateway_reset_http_resource, gateway_http_service,
		     "/reset", &gateway_reset_resource);

int gateway_http_server_start(void)
{
	int ret = http_server_start();

	if (ret < 0) {
		LOG_ERR("HTTP server start failed (%d)", ret);
		return ret;
	}

	LOG_INF("HTTP server listening on port %u", gateway_http_port);
	return 0;
}
