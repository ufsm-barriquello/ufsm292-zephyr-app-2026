/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include <app/gateway/memory_table.h>

static struct gateway_memory memory;

static void *gateway_memory_setup(void)
{
        gateway_memory_init(&memory);
        return NULL;
}

static void gateway_memory_before(void *fixture)
{
        ARG_UNUSED(fixture);
        gateway_memory_init(&memory);
}

ZTEST(gateway_memory, test_empty_table)
{
        struct gateway_memory_entry entry;

        zassert_equal(gateway_memory_get(&memory, 10, &entry),
                      -ENOENT,
                      "empty table should not contain node 10");
}

ZTEST(gateway_memory, test_store_and_get_reading)
{
        struct sensor_reading reading = {
                .node_id = 7,
                .seq = 10,
                .light = 1234,
                .temp_c_x100 = 2350,
                .accel = { 1, 2, 3 },
                .uptime_ms = 5000,
                .flags = 1,
        };
        struct gateway_memory_entry entry;

        zassert_ok(gateway_memory_update(&memory, &reading, 9000),
                   "update failed");

        zassert_ok(gateway_memory_get(&memory, 7, &entry),
                   "stored node was not found");

        zassert_true(entry.valid, "entry should be valid");
        zassert_equal(entry.reading.node_id, 7, "wrong node_id");
        zassert_equal(entry.reading.seq, 10, "wrong seq");
        zassert_equal(entry.reading.light, 1234, "wrong light");
        zassert_equal(entry.reading.temp_c_x100, 2350, "wrong temperature");
        zassert_equal(entry.reading.accel[0], 1, "wrong accel[0]");
        zassert_equal(entry.reading.accel[1], 2, "wrong accel[1]");
        zassert_equal(entry.reading.accel[2], 3, "wrong accel[2]");
        zassert_equal(entry.reading.uptime_ms, 5000, "wrong uptime");
        zassert_equal(entry.reading.flags, 1, "wrong flags");
        zassert_equal(entry.received_at_ms, 9000, "wrong reception timestamp");
}

ZTEST(gateway_memory, test_new_reading_replaces_old_reading)
{
        struct sensor_reading first = {
                .node_id = 3,
                .seq = 1,
                .light = 100,
        };
        struct sensor_reading second = {
                .node_id = 3,
                .seq = 2,
                .light = 200,
        };
        struct gateway_memory_entry entry;

        zassert_ok(gateway_memory_update(&memory, &first, 1000),
                   "first update failed");

        zassert_ok(gateway_memory_update(&memory, &second, 2000),
                   "second update failed");

        zassert_ok(gateway_memory_get(&memory, 3, &entry),
                   "node was not found");

        zassert_equal(entry.reading.seq, 2,
                      "latest sequence was not stored");
        zassert_equal(entry.reading.light, 200,
                      "latest reading was not stored");
        zassert_equal(entry.received_at_ms, 2000,
                      "latest timestamp was not stored");
}

ZTEST(gateway_memory, test_nodes_are_independent)
{
        struct sensor_reading node_a = {
                .node_id = 1,
                .seq = 10,
                .light = 111,
        };
        struct sensor_reading node_b = {
                .node_id = 2,
                .seq = 20,
                .light = 222,
        };
        struct gateway_memory_entry entry;

        zassert_ok(gateway_memory_update(&memory, &node_a, 100),
                   "node 1 update failed");
        zassert_ok(gateway_memory_update(&memory, &node_b, 200),
                   "node 2 update failed");

        zassert_ok(gateway_memory_get(&memory, 1, &entry),
                   "node 1 not found");
        zassert_equal(entry.reading.light, 111, "node 1 data changed");

        zassert_ok(gateway_memory_get(&memory, 2, &entry),
                   "node 2 not found");
        zassert_equal(entry.reading.light, 222, "node 2 data changed");
}

ZTEST(gateway_memory, test_node_id_boundaries)
{
        struct sensor_reading node_zero = {
                .node_id = 0,
                .seq = 1,
        };
        struct sensor_reading node_max = {
                .node_id = 255,
                .seq = 2,
        };
        struct gateway_memory_entry entry;

        zassert_ok(gateway_memory_update(&memory, &node_zero, 10),
                   "node 0 rejected");
        zassert_ok(gateway_memory_update(&memory, &node_max, 20),
                   "node 255 rejected");

        zassert_ok(gateway_memory_get(&memory, 0, &entry),
                   "node 0 not found");
        zassert_equal(entry.reading.seq, 1, "wrong node 0 data");

        zassert_ok(gateway_memory_get(&memory, 255, &entry),
                   "node 255 not found");
        zassert_equal(entry.reading.seq, 2, "wrong node 255 data");
}

ZTEST(gateway_memory, test_full_table)
{
        struct sensor_reading reading = { .node_id = 255, .seq = 1 };
        struct gateway_memory_entry entry;

        zassert_ok(gateway_memory_update(&memory, &reading, 100));
        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES - 1; i++) {
                reading.node_id = i;
                zassert_ok(gateway_memory_update(&memory, &reading, 100));
        }

        reading.node_id = 200;
        zassert_equal(gateway_memory_update(&memory, &reading, 200), -ENOSPC);
        zassert_equal(gateway_memory_get(&memory, 200, &entry), -ENOENT);

        reading.node_id = 255;
        reading.seq = 2;
        zassert_ok(gateway_memory_update(&memory, &reading, 300));
        zassert_ok(gateway_memory_get(&memory, 255, &entry));
        zassert_equal(entry.reading.seq, 2);
        zassert_equal(entry.received_at_ms, 300);
        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES - 1; i++) {
                zassert_ok(gateway_memory_get(&memory, i, &entry));
                zassert_equal(entry.reading.seq, 1);
                zassert_equal(entry.received_at_ms, 100);
        }

        gateway_memory_init(&memory);
        reading.node_id = 200;
        zassert_ok(gateway_memory_update(&memory, &reading, 400));
        zassert_ok(gateway_memory_get(&memory, 200, &entry));
        zassert_equal(gateway_memory_get(&memory, 255, &entry), -ENOENT);
}

ZTEST(gateway_memory, test_request_helpers_empty)
{
        struct gateway_memory_entry entries[GATEWAY_MEMORY_MAX_NODES];
        struct gateway_memory_stats stats = { .valid_nodes = 99, .online_nodes = 99 };

        zassert_equal(gateway_memory_get_all(&memory, entries, 0), 0);
        zassert_ok(gateway_memory_get_stats(&memory, 1000, 100, &stats));
        zassert_equal(stats.valid_nodes, 0);
        zassert_equal(stats.online_nodes, 0);
}

ZTEST(gateway_memory, test_snapshot_and_online_counts)
{
        struct sensor_reading reading = { .node_id = 255, .seq = 1 };
        struct gateway_memory_entry entries[GATEWAY_MEMORY_MAX_NODES];
        struct gateway_memory_stats stats;

        zassert_ok(gateway_memory_update(&memory, &reading, 1000));
        reading.node_id = 0;
        zassert_ok(gateway_memory_update(&memory, &reading, 899));
        reading.node_id = 42;
        zassert_ok(gateway_memory_update(&memory, &reading, 900));
        zassert_ok(gateway_memory_get_stats(&memory, 1000, 100, &stats));
        zassert_equal(stats.valid_nodes, 3);
        zassert_equal(stats.online_nodes, 2, "timeout boundary must be online");

        /* Include stale entries, skip holes, and return copies, not aliases. */
        memory.entries[1].valid = false;
        memory.entries[5] = memory.entries[2];
        memory.entries[2].valid = false;
        zassert_equal(gateway_memory_get_all(&memory, entries, 2), 2);
        zassert_equal(entries[0].reading.node_id, 255);
        zassert_equal(entries[1].reading.node_id, 42);
        zassert_equal(entries[1].received_at_ms, 900);
        zassert_ok(gateway_memory_get_stats(&memory, 2000, 100, &stats));
        zassert_equal(stats.valid_nodes, 2);
        zassert_equal(stats.online_nodes, 0);

        reading.seq = 2;
        zassert_ok(gateway_memory_update(&memory, &reading, 2000));
        zassert_equal(entries[1].reading.seq, 1, "snapshot changed after update");
        zassert_ok(gateway_memory_get_stats(&memory, 2000, 100, &stats));
        zassert_equal(stats.valid_nodes, 2);
        zassert_equal(stats.online_nodes, 1, "fresh reception must restore online status");
}

ZTEST(gateway_memory, test_snapshot_capacity)
{
        struct sensor_reading reading = { 0 };
        struct gateway_memory_entry entries[GATEWAY_MEMORY_MAX_NODES];
        struct gateway_memory_stats stats;

        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES; i++) {
                reading.node_id = i;
                zassert_ok(gateway_memory_update(&memory, &reading, 10));
                entries[i].received_at_ms = 1234;
        }
        zassert_equal(gateway_memory_get_all(&memory, entries, GATEWAY_MEMORY_MAX_NODES - 1),
                      -ENOSPC);
        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES; i++) {
                zassert_equal(entries[i].received_at_ms, 1234, "partial snapshot written");
        }
        zassert_equal(gateway_memory_get_all(&memory, entries, GATEWAY_MEMORY_MAX_NODES),
                      GATEWAY_MEMORY_MAX_NODES);
        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES; i++) {
                zassert_equal(entries[i].reading.node_id, i);
        }
        zassert_ok(gateway_memory_get_stats(&memory, 10, 0, &stats));
        zassert_equal(stats.valid_nodes, GATEWAY_MEMORY_MAX_NODES);
        zassert_equal(stats.online_nodes, GATEWAY_MEMORY_MAX_NODES);
}

ZTEST(gateway_memory, test_online_uptime_wrap)
{
        struct sensor_reading reading = { .node_id = 255 };
        struct gateway_memory_stats stats;

        zassert_ok(gateway_memory_update(&memory, &reading, UINT32_MAX - 49));
        zassert_ok(gateway_memory_get_stats(&memory, 50, 100, &stats));
        zassert_equal(stats.online_nodes, 1);
        zassert_ok(gateway_memory_get_stats(&memory, 51, 100, &stats));
        zassert_equal(stats.valid_nodes, 1);
        zassert_equal(stats.online_nodes, 0);
}

ZTEST(gateway_memory, test_null_arguments)
{
        struct sensor_reading reading = { 0 };
        struct gateway_memory_entry entry;
        struct gateway_memory_stats stats;

        zassert_equal(gateway_memory_update(NULL, &reading, 0),
                      -EINVAL, "NULL memory accepted");
        zassert_equal(gateway_memory_update(&memory, NULL, 0),
                      -EINVAL, "NULL reading accepted");

        zassert_equal(gateway_memory_get(NULL, 0, &entry),
                      -EINVAL, "NULL memory accepted");
        zassert_equal(gateway_memory_get(&memory, 0, NULL),
                      -EINVAL, "NULL entry accepted");
        zassert_equal(gateway_memory_get_all(NULL, &entry, 1), -EINVAL);
        zassert_equal(gateway_memory_get_all(&memory, NULL, 1), -EINVAL);
        zassert_equal(gateway_memory_get_stats(NULL, 0, 100, &stats), -EINVAL);
        zassert_equal(gateway_memory_get_stats(&memory, 0, 100, NULL), -EINVAL);
}

ZTEST_SUITE(gateway_memory, NULL,
            gateway_memory_setup,
            gateway_memory_before,
            NULL, NULL);
