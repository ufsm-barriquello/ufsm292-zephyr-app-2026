/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <zephyr/kernel.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <app/gateway/sensors_json.h>
#include <app/gateway/sensors_response.h>
#include "http_server.h"

/* One connection at a time; storage remains owned until send completes. */
static char response[GATEWAY_SENSORS_JSON_MAX_SIZE];

struct request {
	char path[32];
	size_t path_len;
	bool complete;
};

static void reboot_gateway(struct k_work *work)
{
	ARG_UNUSED(work);
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(reset_work, reboot_gateway);

static int request_url(struct http_parser *parser, const char *data, size_t len)
{
	struct request *request = parser->data;

	if (len >= sizeof(request->path) - request->path_len) {
		return -1;
	}
	memcpy(request->path + request->path_len, data, len);
	request->path_len += len;
	request->path[request->path_len] = '\0';
	return 0;
}

static int request_complete(struct http_parser *parser)
{
	struct request *request = parser->data;

	request->complete = true;
	/* Ignore pipelined requests: every response closes the connection. */
	http_parser_pause(parser, 1);
	return 0;
}

static const struct http_parser_settings parser_settings = {
	.on_url = request_url,
	.on_message_complete = request_complete,
};

static int send_all(int fd, const char *data, size_t len)
{
	while (len > 0U) {
		ssize_t sent = zsock_send(fd, data, len, 0);

		if (sent < 0) {
			return -errno;
		}
		if (sent == 0) {
			return -EIO;
		}
		data += sent;
		len -= sent;
	}
	return 0;
}

static int send_response(int fd, unsigned int status, const char *reason,
			 const char *body, size_t len, const char *allow)
{
	char header[192];
	int count = snprintk(header, sizeof(header),
		"HTTP/1.1 %u %s\r\nContent-Type: application/json\r\n"
		"Content-Length: %u\r\nConnection: close\r\n%s\r\n",
		status, reason, (unsigned int)len, allow);
	int ret;

	if (count < 0 || (size_t)count >= sizeof(header)) {
		return -ENOSPC;
	}
	ret = send_all(fd, header, count);
	return ret < 0 ? ret : send_all(fd, body, len);
}

static void serve_client(int fd)
{
	struct http_parser parser;
	struct request request = { 0 };
	struct zsock_pollfd pollfd = { .fd = fd, .events = ZSOCK_POLLIN };
	struct timeval timeout = { .tv_sec = 5 };
	int64_t deadline = k_uptime_get() + 10000;
	char input[128];
	size_t received = 0;
	bool reset = false;
	int len;

	if (zsock_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
		return;
	}
	http_parser_init(&parser, HTTP_REQUEST);
	parser.data = &request;
	while (!request.complete) {
		int64_t remaining = deadline - k_uptime_get();
		int ret;

		if (remaining <= 0) {
			return;
		}
		ret = zsock_poll(&pollfd, 1, (int)remaining);
		if (ret <= 0 || (pollfd.revents & ZSOCK_POLLIN) == 0) {
			return;
		}
		len = zsock_recv(fd, input, sizeof(input), 0);
		if (len <= 0) {
			return;
		}
		received += len;
		if (received > 2048U) {
			send_response(fd, 413, "Content Too Large", "{}", 2, "");
			return;
		}
		http_parser_execute(&parser, &parser_settings, input, len);
		if (HTTP_PARSER_ERRNO(&parser) != HPE_OK &&
		    HTTP_PARSER_ERRNO(&parser) != HPE_PAUSED) {
			send_response(fd, 400, "Bad Request", "{}", 2, "");
			return;
		}
	}

	if (strcmp(request.path, "/sensors") == 0 || strcmp(request.path, "/health") == 0) {
		if (parser.method != HTTP_GET) {
			send_response(fd, 405, "Method Not Allowed", "{}", 2, "Allow: GET\r\n");
			return;
		}
		len = strcmp(request.path, "/sensors") == 0 ?
			gateway_sensors_json_response(response, sizeof(response)) :
			gateway_health_json_response(response, sizeof(response));
	} else if (strcmp(request.path, "/reset") == 0) {
		if (parser.method != HTTP_POST) {
			send_response(fd, 405, "Method Not Allowed", "{}", 2, "Allow: POST\r\n");
			return;
		}
		reset = true;
		memcpy(response, "{\"status\":\"restarting\"}", sizeof("{\"status\":\"restarting\"}"));
		len = sizeof("{\"status\":\"restarting\"}") - 1;
	} else {
		send_response(fd, 404, "Not Found", "{}", 2, "");
		return;
	}
	if (len < 0) {
		send_response(fd, 500, "Internal Server Error", "{}", 2, "");
		return;
	}
	if (send_response(fd, reset ? 202 : 200, reset ? "Accepted" : "OK",
			  response, len, "") == 0 && reset) {
		/* Queued successfully, not proof of delivery or a TCP acknowledgment. */
		k_work_schedule(&reset_work, K_MSEC(100));
	}
}

int gateway_http_run(void)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_GATEWAY_HTTP_PORT),
		.sin_addr.s_addr = INADDR_ANY,
	};
	int listener = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	int ret;

	if (listener < 0) {
		return -errno;
	}
	if (zsock_bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    zsock_listen(listener, 1) < 0) {
		ret = -errno;
		zsock_close(listener);
		return ret;
	}
	while (true) {
		int client = zsock_accept(listener, NULL, NULL);

		if (client < 0) {
			if (errno == EINTR) {
				continue;
			}
			ret = -errno;
			break;
		}
		serve_client(client);
		zsock_close(client);
	}
	zsock_close(listener);
	return ret;
}
