/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/ieee802154_radio.h>
#include <zephyr/net/ethernet.h>
#include <app/lib/sensor_frame.h>
#include "radio_socket.h"

static uint8_t transmitted[RADIO_SOCKET_FRAME_MAX];
static size_t transmitted_len;
static K_SEM_DEFINE(tx_done, 0, 1);
static uint8_t link_addr[8] = { 2, 0, 0, 0, 0, 0, 0, 1 };
static K_THREAD_STACK_DEFINE(rx_stack, 2048);
static struct k_thread rx_thread;
static int blocking_rx_result;
static uint8_t blocking_rx_frame[RADIO_SOCKET_FRAME_MAX];

static void blocking_rx(void *sock, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	blocking_rx_result = radio_socket_recv(sock, blocking_rx_frame,
					       sizeof(blocking_rx_frame), 0);
}

static enum ieee802154_hw_caps capabilities(const struct device *dev)
{
	ARG_UNUSED(dev);
	return IEEE802154_HW_FCS | IEEE802154_HW_CSMA;
}

static int start_stop(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int txpower(const struct device *dev, int16_t dbm)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(dbm);
	return 0;
}

static int transmit(const struct device *dev, enum ieee802154_tx_mode mode,
		    struct net_pkt *pkt, struct net_buf *frag)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(mode);
	ARG_UNUSED(pkt);
	transmitted_len = frag->len;
	memcpy(transmitted, frag->data, frag->len);
	k_sem_give(&tx_done);
	return 0;
}

static void iface_init(struct net_if *iface)
{
	net_if_set_link_addr(iface, link_addr, sizeof(link_addr), NET_LINK_IEEE802154);
	ieee802154_init(iface);
	((struct ieee802154_context *)net_if_l2_data(iface))->channel = 15;
}

static const struct ieee802154_radio_api api = {
	.iface_api.init = iface_init,
	.get_capabilities = capabilities,
	.start = start_stop,
	.stop = start_stop,
	.set_txpower = txpower,
	.tx = transmit,
};

NET_DEVICE_INIT(test_radio, "test_radio", NULL, NULL, NULL, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api, IEEE802154_L2,
		NET_L2_GET_CTX_TYPE(IEEE802154_L2), IEEE802154_MTU);
NET_DEVICE_INIT(other_radio, "other_radio", NULL, NULL, NULL, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api, IEEE802154_L2,
		NET_L2_GET_CTX_TYPE(IEEE802154_L2), IEEE802154_MTU);

static uint8_t eth_addr[6] = { 2, 0, 0, 0, 0, 2 };

static void eth_iface_init(struct net_if *iface)
{
	net_if_set_link_addr(iface, eth_addr, sizeof(eth_addr), NET_LINK_ETHERNET);
	ethernet_init(iface);
}

static int eth_send(const struct device *dev, struct net_pkt *pkt)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(pkt);
	return 0;
}

static const struct ethernet_api eth_api = {
	.iface_api.init = eth_iface_init,
	.send = eth_send,
};

ETH_NET_DEVICE_INIT(test_eth, "test_eth", NULL, NULL, NULL, NULL,
		    CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &eth_api, NET_ETH_MTU);

static void inject(struct net_if *iface, const uint8_t *frame, size_t len, bool fragmented)
{
	struct net_pkt *pkt = net_pkt_rx_alloc(K_SECONDS(1));

	zassert_not_null(pkt);
	net_pkt_set_iface(pkt, iface);
	/* Normal IEEE802154 L2 requires a contiguous RX buffer. The synthetic
	 * fragmented case bypasses L2 to test socket copying independently.
	 */
	net_pkt_set_l2_processed(pkt, fragmented);
	for (size_t offset = 0; offset < len;) {
		size_t part = fragmented ? MIN(5U, len - offset) : len;
		struct net_buf *frag = net_pkt_get_frag(pkt, part, K_SECONDS(1));

		zassert_not_null(frag);
		net_buf_add_mem(frag, frame + offset, part);
		net_pkt_frag_add(pkt, frag);
		offset += part;
	}
	zassert_ok(net_recv_data(iface, pkt));
	/* Let the real network RX thread deliver the packet to the socket. */
	k_sleep(K_MSEC(20));
}

ZTEST(radio_socket, test_codec_roundtrip_and_isolation)
{
	struct net_if *iface = net_if_lookup_by_dev(DEVICE_GET(test_radio));
	struct net_if *other = net_if_lookup_by_dev(DEVICE_GET(other_radio));
	struct radio_socket sock;
	struct radio_socket rejected;
	struct net_if *eth = net_if_lookup_by_dev(DEVICE_GET(test_eth));
	uint8_t eth_frame[60] = { [12] = 0x88, [13] = 0xb5 };
	struct sensor_frame_cfg cfg = { .pan_id = 0xcafe, .src_short_addr = 1,
		.dst_short_addr = 0xffff };
	struct sensor_reading input = { .node_id = 1, .seq = 42, .light = 123 };
	struct sensor_reading output;
	uint8_t frame[RADIO_SOCKET_FRAME_MAX], received[RADIO_SOCKET_FRAME_MAX];
	int len = sensor_frame_encode(frame, sizeof(frame), &cfg, &input);

	zassert_true(len > 16);
	zassert_ok(net_if_up(iface));
	zassert_ok(net_if_up(other));
	zassert_ok(radio_socket_open(&sock, iface));
	zassert_equal(radio_socket_open(&rejected, eth), -ENODEV);
	memcpy(eth_frame, eth_addr, sizeof(eth_addr));
	inject(eth, eth_frame, sizeof(eth_frame), false);
	zassert_equal(radio_socket_recv(&sock, received, sizeof(received),
				       ZSOCK_MSG_DONTWAIT), -EAGAIN);
	zassert_equal(net_send_raw(&sock, frame, len), len);
	zassert_ok(k_sem_take(&tx_done, K_SECONDS(1)));
	zassert_equal(transmitted_len, len);
	zassert_mem_equal(transmitted, frame, len);

	inject(other, frame, len, false);
	zassert_equal(radio_socket_recv(&sock, received, sizeof(received),
				       ZSOCK_MSG_DONTWAIT), -EAGAIN);
	inject(iface, frame, len, false);
	zassert_equal(radio_socket_recv(&sock, received, sizeof(received),
				       ZSOCK_MSG_DONTWAIT), len);
	zassert_mem_equal(received, frame, len);
	zassert_ok(sensor_frame_decode(received, len, &output));
	zassert_equal(output.seq, input.seq);
	zassert_equal(output.light, input.light);

	inject(iface, frame, len, true);
	zassert_equal(radio_socket_recv(&sock, received, sizeof(received),
				       ZSOCK_MSG_DONTWAIT), len);
	zassert_mem_equal(received, frame, len);

	/* TX must work on this same fd while another thread is waiting for RX. */
	k_tid_t tid = k_thread_create(&rx_thread, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack),
				     blocking_rx, &sock, NULL, NULL, 5, 0, K_NO_WAIT);
	k_sleep(K_MSEC(20));
	zassert_equal(net_send_raw(&sock, frame, len), len);
	zassert_ok(k_sem_take(&tx_done, K_SECONDS(1)));
	inject(iface, frame, len, false);
	zassert_ok(k_thread_join(tid, K_SECONDS(1)));
	zassert_equal(blocking_rx_result, len);
	zassert_mem_equal(blocking_rx_frame, frame, len);
	inject(iface, frame, len, false);
	zassert_equal(radio_socket_recv(&sock, received, 8, ZSOCK_MSG_DONTWAIT),
		      -EMSGSIZE);
	zassert_equal(net_send_raw(&sock, frame, sizeof(frame) + 1), -EMSGSIZE);
	zassert_equal(net_send_raw(&sock, NULL, 0), -EINVAL);
	radio_socket_close(&sock);
	zassert_equal(net_send_raw(&sock, frame, len), -EBADF);
	zassert_equal(radio_socket_open(&sock, NULL), -ENODEV);
}

ZTEST_SUITE(radio_socket, NULL, NULL, NULL, NULL, NULL);
