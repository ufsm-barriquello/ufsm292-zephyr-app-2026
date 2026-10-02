/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <zephyr/net/ethernet.h>
#include "radio_socket.h"

int radio_socket_open(struct radio_socket *sock, struct net_if *iface)
{
	int ret;

	sock->fd = -1;
	if (iface == NULL || net_if_get_link_addr(iface)->type != NET_LINK_IEEE802154) {
		return -ENODEV;
	}

	sock->addr = (struct sockaddr_ll) {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_ALL),
		.sll_ifindex = net_if_get_by_iface(iface),
	};
	sock->fd = zsock_socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (sock->fd < 0) {
		return -errno;
	}
	if (zsock_bind(sock->fd, (struct sockaddr *)&sock->addr, sizeof(sock->addr)) < 0) {
		ret = -errno;
		radio_socket_close(sock);
		return ret;
	}
	return 0;
}

void radio_socket_close(struct radio_socket *sock)
{
	if (sock->fd >= 0) {
		zsock_close(sock->fd);
		sock->fd = -1;
	}
}

int net_send_raw(struct radio_socket *sock, const uint8_t *psdu, size_t len)
{
	ssize_t ret;

	if (psdu == NULL || len == 0) {
		return -EINVAL;
	}
	if (len > RADIO_SOCKET_FRAME_MAX) {
		return -EMSGSIZE;
	}
	/* Packet sends are atomic: never retry a partial frame as another frame.
	 * Do not wait indefinitely for network buffer availability.
	 */
	ret = zsock_sendto(sock->fd, psdu, len, ZSOCK_MSG_DONTWAIT,
			   (struct sockaddr *)&sock->addr, sizeof(sock->addr));
	return ret < 0 ? -errno : (ret == len ? (int)ret : -EIO);
}

int radio_socket_recv(struct radio_socket *sock, uint8_t *psdu, size_t cap, int flags)
{
	struct iovec iov = { .iov_base = psdu, .iov_len = cap };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	ssize_t ret;

	if (sock->fd < 0) {
		return -EBADF;
	}
	if (psdu == NULL || cap == 0) {
		return -EINVAL;
	}
	/* Packet recvmsg waits with the fd lock held on this Zephyr version.
	 * Poll first, then receive without waiting so idle RX cannot block TX
	 * through the same socket. This transport has exactly one RX consumer.
	 */
	if (!(flags & ZSOCK_MSG_DONTWAIT)) {
		struct zsock_pollfd pfd = { .fd = sock->fd, .events = ZSOCK_POLLIN };

		ret = zsock_poll(&pfd, 1, -1);
		if (ret < 0) {
			return -errno;
		}
		if (pfd.revents & ZSOCK_POLLNVAL) {
			return -EBADF;
		}
		if (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP)) {
			return -EIO;
		}
	}
	ret = zsock_recvmsg(sock->fd, &msg, flags | ZSOCK_MSG_DONTWAIT);
	if (ret < 0) {
		return -errno;
	}
	if ((msg.msg_flags & ZSOCK_MSG_TRUNC) || ret > RADIO_SOCKET_FRAME_MAX) {
		return -EMSGSIZE;
	}
	return ret;
}
