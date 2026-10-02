/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GATEWAY_RADIO_SOCKET_H_
#define GATEWAY_RADIO_SOCKET_H_

#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>

/* Full IEEE 802.15.4 PSDUs, without FCS. Codec APIs remain independent of
 * Zephyr. Open before use; one RX consumer, TX may run in another thread.
 * Close only after both users have stopped. Functions return negative errno.
 */
#define RADIO_SOCKET_FRAME_MAX 125U

struct radio_socket {
	int fd;
	struct sockaddr_ll addr;
};

int radio_socket_open(struct radio_socket *sock, struct net_if *iface);
void radio_socket_close(struct radio_socket *sock);
int net_send_raw(struct radio_socket *sock, const uint8_t *psdu, size_t len);
int radio_socket_recv(struct radio_socket *sock, uint8_t *psdu, size_t cap,
		      int flags);

#endif
