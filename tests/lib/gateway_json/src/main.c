/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/gateway/memory_table.h>
#include <app/gateway/sensors_json.h>

static struct gateway_memory memory;
static struct k_mutex memory_lock;
static char output[GATEWAY_SENSORS_JSON_MAX_SIZE];

static void gateway_json_before(void *fixture)
{
	ARG_UNUSED(fixture);
	gateway_memory_init(&memory);
	k_mutex_init(&memory_lock);
}

ZTEST(gateway_json, test_empty_table)
{
	zassert_equal(gateway_sensors_json_encode(&memory, &memory_lock,
						 output, sizeof(output)),
		      2, "empty table should encode as an empty array");
	zassert_str_equal(output, "[]");
}

ZTEST(gateway_json, test_single_sensor_format_and_json)
{
	struct sensor_reading reading = {
		.node_id = 1,
		.light = 320,
		.temp_c_x100 = -5,
		.accel = { 1, -2, 980 },
	};
	const char expected[] =
		"[{\"node_id\":1,\"light\":320,\"temp_c\":-0.05,"
		"\"accel\":[1,-2,980],\"last_seen_ms\":1234}]";

	zassert_ok(gateway_memory_update(&memory, &reading, 1234));
	zassert_equal(gateway_sensors_json_encode(&memory, &memory_lock,
						 output, sizeof(output)),
		      sizeof(expected) - 1, "wrong JSON length");
	zassert_str_equal(output, expected);
}

ZTEST(gateway_json, test_multiple_sensors_and_latest_update)
{
	struct sensor_reading reading = {
		.node_id = 1,
		.light = 100,
		.temp_c_x100 = 2450,
		.accel = { 0, 2, 980 },
	};
	const char expected[] =
		"[{\"node_id\":1,\"light\":300,\"temp_c\":24.50,"
		"\"accel\":[0,2,980],\"last_seen_ms\":300},"
		"{\"node_id\":2,\"light\":200,\"temp_c\":-1.25,"
		"\"accel\":[-10,20,-30],\"last_seen_ms\":200}]";

	zassert_ok(gateway_memory_update(&memory, &reading, 100));
	reading.node_id = 2;
	reading.light = 200;
	reading.temp_c_x100 = -125;
	reading.accel[0] = -10;
	reading.accel[1] = 20;
	reading.accel[2] = -30;
	zassert_ok(gateway_memory_update(&memory, &reading, 200));
	reading.node_id = 1;
	reading.light = 300;
	reading.temp_c_x100 = 2450;
	reading.accel[0] = 0;
	reading.accel[1] = 2;
	reading.accel[2] = 980;
	zassert_ok(gateway_memory_update(&memory, &reading, 300));

	zassert_equal(gateway_sensors_json_encode(&memory, &memory_lock,
						 output, sizeof(output)),
		      sizeof(expected) - 1, "wrong JSON length");
	zassert_str_equal(output, expected);
}

ZTEST(gateway_json, test_buffer_too_small_is_not_partial_json)
{
	struct sensor_reading reading = { .node_id = 7 };
	char small_output[8] = "stale";

	zassert_ok(gateway_memory_update(&memory, &reading, 1));
	zassert_equal(gateway_sensors_json_encode(&memory, &memory_lock,
						 small_output, sizeof(small_output)),
		      -ENOSPC, "undersized buffer should fail");
	zassert_str_equal(small_output, "");
}

ZTEST(gateway_json, test_exactly_sized_buffer)
{
	struct sensor_reading reading = {
		.node_id = 5,
		.light = 42,
		.temp_c_x100 = 2150,
		.accel = { 1, 2, 3 },
	};
	const char expected[] =
		"[{\"node_id\":5,\"light\":42,\"temp_c\":21.50,"
		"\"accel\":[1,2,3],\"last_seen_ms\":99}]";

	zassert_ok(gateway_memory_update(&memory, &reading, 99));
	zassert_equal(gateway_sensors_json_encode(&memory, &memory_lock,
						 output, sizeof(expected)),
		      sizeof(expected) - 1, "exact-sized buffer should succeed");
	zassert_str_equal(output, expected);
}

ZTEST(gateway_json, test_full_table_fits_maximum_response_buffer)
{
	struct sensor_reading reading = {
		.light = UINT16_MAX,
		.temp_c_x100 = INT16_MIN,
		.accel = { INT16_MIN, INT16_MAX, INT16_MIN },
	};
	int length;

	for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES; i++) {
		reading.node_id = (i == 0U) ? 0U : (uint8_t)(UINT8_MAX - i + 1U);
		zassert_ok(gateway_memory_update(&memory, &reading, UINT32_MAX));
	}

	length = gateway_sensors_json_encode(&memory, &memory_lock,
					    output, sizeof(output));
	zassert_true(length > 0, "full table did not encode");
	zassert_true((size_t)length < sizeof(output),
		     "maximum response exceeded its advertised buffer size");
	zassert_equal(output[0], '[');
	zassert_equal(output[length - 1], ']');
	zassert_equal(output[length], '\0');
}

ZTEST(gateway_json, test_invalid_arguments)
{
	zassert_equal(gateway_sensors_json_encode(NULL, &memory_lock,
						 output, sizeof(output)), -EINVAL);
	zassert_equal(gateway_sensors_json_encode(&memory, NULL,
						 output, sizeof(output)), -EINVAL);
	zassert_equal(gateway_sensors_json_encode(&memory, &memory_lock,
						 NULL, sizeof(output)), -EINVAL);
}

ZTEST_SUITE(gateway_json, NULL, NULL, gateway_json_before, NULL, NULL);