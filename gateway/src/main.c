/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gateway bring-up image for samr21_xpro.
 *
 * Raw-socket 802.15.4 receiver: the radio is brought up promiscuous on
 * channel 15 with our PAN ID (0xCAFE) set in the hardware filter. Frames whose
 * MAC payload is
 * exactly the 18-byte sensor payload decode via sensor_frame_decode() and
 * are printed to the console; anything else is just counted.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/ieee802154_radio.h>
#include <zephyr/net/ieee802154.h>
#include <zephyr/net/ieee802154_mgmt.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/sys/atomic.h>
#include "radio_socket.h"

#include <app/lib/sensor_frame.h>
#include <app/gateway/memory_table.h>
#include <app/gateway/sensors_json.h>
#include <app/gateway/sensors_response.h>
#include <errno.h>
#include <inttypes.h>

#include <zephyr/sys/printk.h>

#include "http_server.h"

#include <zephyr/app_version.h>

LOG_MODULE_REGISTER(gateway, CONFIG_GATEWAY_LOG_LEVEL);

#define GW_CHANNEL 15
#define GW_PAN_ID  0xCAFE

static const struct device *const radio =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_ieee802154));

static struct gateway_memory gateway_memory;
static atomic_t rx_ok;
static atomic_t rx_other;
static struct radio_socket radio_sock = { .fd = -1 };
static K_SEM_DEFINE(rx_ready, 0, 1);
static atomic_t radio_ready;

/* Ethernet link monitor: logs carrier up/down and current IPv4 address. */
static void eth_event_handler(struct net_mgmt_event_callback *cb,
			      uint64_t event, struct net_if *iface)
{
	char addr[NET_IPV4_ADDR_LEN];
	ARG_UNUSED(cb);
	if (net_if_l2(iface) != &NET_L2_GET_NAME(ETHERNET)) {
		return;
	}
	if (event == NET_EVENT_ETHERNET_CARRIER_ON) {
		LOG_INF("eth: carrier on");
	} else if (event == NET_EVENT_ETHERNET_CARRIER_OFF) {
		LOG_WRN("eth: carrier off");
	} else if (event == NET_EVENT_IPV4_ADDR_ADD) {
		struct net_in_addr *ip = net_if_ipv4_get_global_addr(iface,
								 NET_ADDR_PREFERRED);

		if (ip != NULL && net_addr_ntop(AF_INET, ip, addr, sizeof(addr)) != NULL) {
			LOG_INF("eth: ipv4 %s", addr);
		}
	}
}

static struct net_mgmt_event_callback eth_cb;
static struct net_if *eth_iface;

/* Run after device/network initialization and before the application threads. */
static int eth_setup(void)
{
	net_mgmt_init_event_callback(&eth_cb, eth_event_handler,
				     NET_EVENT_ETHERNET_CARRIER_ON |
				     NET_EVENT_ETHERNET_CARRIER_OFF |
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&eth_cb);
	eth_iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
	if (eth_iface == NULL) {
		LOG_WRN("eth: no ethernet interface, monitor idle");
		return -ENODEV;
	}
	if (!device_is_ready(net_if_get_device(eth_iface))) {
		LOG_ERR("eth: controller initialization failed");
		eth_iface = NULL;
		return -ENODEV;
	}
	if (!net_if_is_admin_up(eth_iface)) {
		int ret = net_if_up(eth_iface);

		if (ret < 0) {
			LOG_ERR("eth: net_if_up failed (%d)", ret);
			eth_iface = NULL;
			return ret;
		}
	}
	net_dhcpv4_start(eth_iface);
	LOG_INF("eth: monitor up, carrier=%s admin=%s",
		net_if_is_carrier_ok(eth_iface) ? "ok" : "down",
		net_if_is_admin_up(eth_iface) ? "up" : "down");
	return 0;
}

SYS_INIT(eth_setup, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static K_MUTEX_DEFINE(gateway_memory_lock);

int gateway_sensors_json_response(char *output, size_t capacity)
{
	return gateway_sensors_json_encode(&gateway_memory, &gateway_memory_lock,
					   output, capacity);
}

int gateway_health_json_response(char *output, size_t capacity)
{
	struct gateway_memory_stats stats;
	bool ready;
	int written;

	if (output == NULL) {
		return -EINVAL;
	}
	if (capacity == 0U) {
		return -ENOSPC;
	}

	k_mutex_lock(&gateway_memory_lock, K_FOREVER);
	gateway_memory_get_stats(&gateway_memory, k_uptime_get_32(),
				 CONFIG_GATEWAY_NODE_ONLINE_TIMEOUT_MS, &stats);
	k_mutex_unlock(&gateway_memory_lock);
	ready = atomic_get(&radio_ready) != 0;

	written = snprintk(output, capacity,
			   "{\"status\":\"%s\",\"uptime_ms\":%" PRIu64
			   ",\"online_nodes\":%u}",
			   ready ? "ok" : "degraded",
			   (uint64_t)k_uptime_get(),
			   (unsigned int)stats.online_nodes);
	if (written < 0 || (size_t)written >= capacity) {
		output[0] = '\0';
		return -ENOSPC;
	}
	return written;
}

/* Runs in the RX thread; console output uses a copy outside the table lock. */
static void handle_sensor_reading(const struct sensor_reading *r)
{
	struct gateway_memory_entry entry;
	int ret;
	int get_ret;

	k_mutex_lock(&gateway_memory_lock, K_FOREVER);
	ret = gateway_memory_update(&gateway_memory, r, k_uptime_get_32());
	get_ret = gateway_memory_get(&gateway_memory, r->node_id, &entry);
	k_mutex_unlock(&gateway_memory_lock);

	if (ret < 0) {
		LOG_WRN("Memory update for node %u failed (%d)", r->node_id, ret);
	}
	if (get_ret == 0) {
		printk("memory node=%u seq=%u received_at=%u ms\n",
		       entry.reading.node_id, entry.reading.seq,
		       entry.received_at_ms);
	}

	printk("rx node=%u seq=%u light=%u temp_c_x100=%d "
	       "accel=%d,%d,%d uptime=%u flags=%u\n",
	       r->node_id, r->seq, r->light, r->temp_c_x100,
	       r->accel[0], r->accel[1], r->accel[2],
	       r->uptime_ms, r->flags);
}

/*
 * Socket copies the complete frame (including MAC header) out of all packet
 * fragments. Decode/print cannot block the radio driver's RX thread.
 */
static void receive_frames(void *p1, void *p2, void *p3)
{
	struct sensor_reading r;
	uint8_t psdu[RADIO_SOCKET_FRAME_MAX];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	k_sem_take(&rx_ready, K_FOREVER);
	while (true) {
		int len = radio_socket_recv(&radio_sock, psdu, sizeof(psdu), 0);

		if (len < 0 && len != -EMSGSIZE) {
			LOG_ERR("Raw socket receive failed (%d)", len);
			k_sleep(K_MSEC(100));
			continue;
		}
		if (len >= 0 && sensor_frame_decode(psdu, len, &r) == 0) {
			atomic_inc(&rx_ok);
			handle_sensor_reading(&r);
		} else {
			atomic_inc(&rx_other);
		}
	}
}

K_THREAD_DEFINE(radio_rx_id, 2048, receive_frames, NULL, NULL, NULL, 5, 0, 0);

static void gateway_run(void)
{
	const struct ieee802154_radio_api *api;
	struct net_if *iface;
	uint16_t channel = GW_CHANNEL;
	int ret;

	printk("Gateway %s (samr21_xpro, ch %d, pan 0x%04x)\n",
	       APP_VERSION_STRING, GW_CHANNEL, GW_PAN_ID);

	if (!device_is_ready(radio)) {
		LOG_ERR("Radio not ready");
		return;
	}

	api = (const struct ieee802154_radio_api *)radio->api;
	iface = net_if_lookup_by_dev(radio);
	ret = radio_socket_open(&radio_sock, iface);
	if (ret < 0) {
		LOG_ERR("Raw socket open failed (%d)", ret);
		return;
	}
	/* Set both L2 state and hardware: L2 refuses to start without a channel. */
	ret = net_mgmt(NET_REQUEST_IEEE802154_SET_CHANNEL, iface, &channel, sizeof(channel));
	if (ret < 0) {
		LOG_ERR("set_channel(%d) failed (%d)", GW_CHANNEL, ret);
		radio_socket_close(&radio_sock);
		return;
	}

	/* Accept only our PAN and matching unicast/broadcast destinations. */
	{
		struct ieee802154_filter f = { .pan_id = GW_PAN_ID };

		ret = api->filter(radio, true, IEEE802154_FILTER_TYPE_PAN_ID, &f);
		if (ret < 0) {
			LOG_ERR("PAN ID filter refused (%d)", ret);
			radio_socket_close(&radio_sock);
			return;
		}
	}

	/* Explicitly restore hardware filtering before starting reception. */
	{
		struct ieee802154_config cfg = { .promiscuous = false };

		ret = api->configure(radio, IEEE802154_CONFIG_PROMISCUOUS, &cfg);
		if (ret < 0) {
			LOG_ERR("Disabling promiscuous mode failed (%d)", ret);
			radio_socket_close(&radio_sock);
			return;
		}
	}

	ret = net_if_up(iface);
	if (ret < 0) {
		LOG_ERR("Radio interface up failed (%d)", ret);
		radio_socket_close(&radio_sock);
		return;
	}
	atomic_set(&radio_ready, 1);
	k_sem_give(&rx_ready);

	LOG_INF("Listening on ch %d, PAN ID 0x%04x, promiscuous off", GW_CHANNEL, GW_PAN_ID);

}

static void gateway_stats(struct k_work *work)
{
	static unsigned int ticks;
	struct gateway_memory_stats stats;

	ticks++;
	k_mutex_lock(&gateway_memory_lock, K_FOREVER);
	gateway_memory_get_stats(&gateway_memory, k_uptime_get_32(),
				 CONFIG_GATEWAY_NODE_ONLINE_TIMEOUT_MS, &stats);
	k_mutex_unlock(&gateway_memory_lock);
	LOG_INF("gw ok=%u other=%u stored=%u active=%u",
		(unsigned int)atomic_get(&rx_ok), (unsigned int)atomic_get(&rx_other),
		(unsigned int)stats.valid_nodes, (unsigned int)stats.online_nodes);
	if (eth_iface != NULL && (ticks % 2) == 0) {
		char addr[NET_IPV4_ADDR_LEN];
		struct net_in_addr *ip = net_if_ipv4_get_global_addr(eth_iface,
								 NET_ADDR_PREFERRED);
		const char *ip_text = "none";

		if (ip != NULL && net_addr_ntop(AF_INET, ip, addr, sizeof(addr)) != NULL) {
			ip_text = addr;
		}
		LOG_INF("eth: carrier=%s admin=%s ip=%s",
			net_if_is_carrier_ok(eth_iface) ? "ok" : "down",
			net_if_is_admin_up(eth_iface) ? "up" : "down", ip_text);
	}
	k_work_schedule(k_work_delayable_from_work(work), K_SECONDS(5));
}

static K_WORK_DELAYABLE_DEFINE(stats_work, gateway_stats);

int main(void)
{
	int ret;

	gateway_run();
	k_work_schedule(&stats_work, K_SECONDS(5));
	/* Radio setup failure still leaves health and recovery available over HTTP. */
	ret = gateway_http_run();
	LOG_ERR("HTTP server stopped (%d)", ret);
	return ret;
}
