/*
 * Microchip KSZ8851SNL: controlador Ethernet 10/100 em SPI, o chip da
 * ATETHERNET1-XPRO. O Zephyr não traz driver para ele.
 *
 * As filas de quadros (QMU) exigem ligar o bit SDA do RXQCR antes e desligar
 * depois, e com o SDA ligado os registradores não respondem. Por isso todo
 * acesso ao SPI passa por `lock`.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT microchip_ksz8851snl

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>

#include "eth_ksz8851snl_priv.h"

LOG_MODULE_REGISTER(eth_ksz8851snl, CONFIG_ETHERNET_LOG_LEVEL);

/*
 * Um spi_buf por fragmento do net_pkt. O limite real é
 * CONFIG_NET_BUF_{RX,TX}_COUNT (12 por padrão); 14 cobre com folga.
 */
#define KSZ_MAX_BUFS 14

struct eth_ksz8851_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec interrupt;
	struct gpio_dt_spec reset;
	bool random_mac;
};

struct eth_ksz8851_data {
	struct net_if *iface;
	uint8_t mac_addr[6];

	/* Serializa o SPI entre a thread de recepção e as transmissões */
	struct k_mutex lock;

	/*
	 * Fora da pilha de propósito: a transmissão roda na workqueue do
	 * sistema (1 KB) e um array local deste tamanho a estourava. Recepção
	 * e transmissão compartilham porque as duas correm sob `lock`.
	 */
	struct spi_buf bufs[KSZ_MAX_BUFS + 3];
	struct k_sem int_sem;
	struct gpio_callback gpio_cb;

	/* Valor corrente do RXQCR sem o SDA, para não reler a cada acesso */
	uint16_t rxqcr;
	uint16_t fid;
	bool link_up;

	/* Diagnóstico: lido da RAM pelo depurador, já que não há console */
	struct {
		uint16_t cider;         /* Chip ID lido no init (0x887x = ok) */
		uint16_t isr;           /* voltas com ISR != 0 */
		uint16_t rxi;           /* interrupções de recepção */
		uint16_t rx_seen;       /* quadros anunciados pelo RXFCTR */
		uint16_t rx_ok;         /* entregues à pilha de rede */
		uint16_t rx_bad_status; /* descartados pelo RXFHSR */
		uint16_t rx_bad_len;    /* comprimento fora de faixa */
		uint16_t rx_no_pkt;     /* sem net_pkt disponível */
		uint16_t rx_err;        /* falha ao ler da RXQ */
		uint16_t rx_reject;     /* a pilha de rede recusou o pacote */
		int16_t rx_reject_err;  /* código do último net_recv_data() < 0 */
		uint16_t rx_ovf;        /* RX FIFO estourou */
		uint16_t isr_last;      /* último ISR diferente de zero */
		uint16_t tx_ok;         /* quadros enfileirados para sair */
		uint16_t tx_full;       /* desistiu: TX FIFO cheia */
		uint16_t spi_err;       /* erro no barramento SPI */

		/* Espelho dos registradores: rxfctr sempre zero quer dizer
		 * que nada chegou ao chip, logo o problema é do cabo.
		 */
		uint16_t p1sr;          /* estado do PHY */
		uint16_t rxcr1;         /* bit0 = recepção habilitada */
		uint16_t isr_reg;       /* ISR na última leitura */
		uint16_t rxfctr;        /* byte alto = quadros esperando */
	} stats;

	K_KERNEL_STACK_MEMBER(thread_stack, CONFIG_ETH_KSZ8851SNL_RX_THREAD_STACK_SIZE);
	struct k_thread thread;
};

/* --- acesso a registradores ---------------------------------------------- */

static int ksz_write16(const struct device *dev, uint16_t reg, uint16_t val)
{
	const struct eth_ksz8851_config *cfg = dev->config;
	uint8_t buf[4] = {
		KSZ_CMD0(KSZ_OP_WR, reg),
		KSZ_CMD1(reg),
		val & 0xff,
		val >> 8,
	};
	const struct spi_buf tx_buf = {.buf = buf, .len = sizeof(buf)};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};

	return spi_write_dt(&cfg->spi, &tx);
}

static uint16_t ksz_read16(const struct device *dev, uint16_t reg)
{
	const struct eth_ksz8851_config *cfg = dev->config;
	uint8_t cmd[2] = {KSZ_CMD0(KSZ_OP_RD, reg), KSZ_CMD1(reg)};
	uint8_t val[2];
	const struct spi_buf tx_buf = {.buf = cmd, .len = sizeof(cmd)};
	const struct spi_buf rx_bufs[] = {
		/* NULL é seguro por estar emparelhado com o spi_buf de
		 * transmissão de mesmo índice; ver a nota em ksz_read_frame().
		 */
		{.buf = NULL, .len = sizeof(cmd)}, /* ecoa o comando: descartar */
		{.buf = val, .len = sizeof(val)},
	};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf_set rx = {.buffers = rx_bufs, .count = ARRAY_SIZE(rx_bufs)};

	if (spi_transceive_dt(&cfg->spi, &tx, &rx) < 0) {
		LOG_ERR("leitura do registrador 0x%02x falhou", reg);
		return 0;
	}

	return sys_get_le16(val);
}

/* Liga/desliga o acesso às filas (SDA). Só com o mutex tomado. */
static int ksz_fifo_access(const struct device *dev, bool on)
{
	struct eth_ksz8851_data *ctx = dev->data;

	return ksz_write16(dev, KSZ_RXQCR, on ? (ctx->rxqcr | RXQCR_SDA) : ctx->rxqcr);
}

/* --- recepção ------------------------------------------------------------- */

/*
 * Lê um quadro da RXQ para os fragmentos de `pkt`, numa transação só: opcode,
 * 8 bytes de status/lixo, o quadro e o enchimento até múltiplo de 4. Quem
 * chama liga e desliga o SDA.
 *
 * Os pedaços descartados vão para um rascunho em vez de `.buf = NULL`: o
 * spi_sam0 só aceita NULL num spi_buf de recepção emparelhado com um de
 * transmissão. Os que sobram no fim caem em spi_sam0_fast_rx(), que
 * desreferencia o ponteiro direto -- com NULL, hard fault.
 */
static int ksz_read_frame(const struct device *dev, struct net_pkt *pkt, uint16_t len)
{
	const struct eth_ksz8851_config *cfg = dev->config;
	struct eth_ksz8851_data *ctx = dev->data;
	struct spi_buf *rx_bufs = ctx->bufs;
	uint8_t op = KSZ_OP_RXFIFO;
	uint8_t scratch[KSZ_RX_HDR_LEN]; /* destino do que é lido e descartado */
	const struct spi_buf tx_buf = {.buf = &op, .len = 1};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
	struct spi_buf_set rx = {.buffers = rx_bufs};
	size_t pad = ROUND_UP(len, 4) - len;
	struct net_buf *frag;
	size_t left = len;
	size_t n = 0;

	rx_bufs[n].buf = scratch; /* eco do opcode */
	rx_bufs[n++].len = 1;
	rx_bufs[n].buf = scratch; /* 4 de status + 4 de lixo */
	rx_bufs[n++].len = KSZ_RX_HDR_LEN;

	for (frag = pkt->buffer; frag != NULL && left > 0; frag = frag->frags) {
		size_t chunk = MIN(left, net_buf_tailroom(frag));

		if (chunk == 0) {
			continue;
		}
		if (n >= KSZ_MAX_BUFS + 2) {
			LOG_ERR("quadro de %u bytes em fragmentos demais", len);
			return -ENOBUFS;
		}
		rx_bufs[n].buf = net_buf_add(frag, chunk);
		rx_bufs[n++].len = chunk;
		left -= chunk;
	}

	if (left > 0) {
		LOG_ERR("buffer curto: faltaram %zu bytes de %u", left, len);
		return -ENOBUFS;
	}

	if (pad > 0) {
		/* pad <= 3, cabe de sobra no rascunho */
		rx_bufs[n].buf = scratch;
		rx_bufs[n++].len = pad;
	}
	rx.count = n;

	return spi_transceive_dt(&cfg->spi, &tx, &rx);
}

/* Descarta o quadro do topo da RXQ sem lê-lo */
static void ksz_drop_frame(const struct device *dev)
{
	struct eth_ksz8851_data *ctx = dev->data;

	ksz_write16(dev, KSZ_RXQCR, ctx->rxqcr | RXQCR_RRXEF);
}

static void ksz_rx(const struct device *dev)
{
	struct eth_ksz8851_data *ctx = dev->data;
	uint8_t frames = ksz_read16(dev, KSZ_RXFCTR) >> RXFCTR_RXFC_SHIFT;

	ctx->stats.rx_seen += frames;

	while (frames-- > 0) {
		uint16_t status = ksz_read16(dev, KSZ_RXFHSR);
		uint16_t len = ksz_read16(dev, KSZ_RXFHBCR) & RXFHBCR_CNT_MASK;
		struct net_pkt *pkt;
		int ret;

		if (!(status & RXFHSR_RXFV) || (status & RXFHSR_ERR)) {
			LOG_DBG("quadro descartado, status 0x%04x", status);
			ctx->stats.rx_bad_status++;
			ksz_drop_frame(dev);
			continue;
		}

		/* O comprimento do cabeçalho inclui o CRC, que não interessa */
		if (len <= KSZ_RX_CRC_LEN || len > NET_ETH_MAX_FRAME_SIZE + KSZ_RX_CRC_LEN) {
			LOG_DBG("comprimento fora de faixa: %u", len);
			ctx->stats.rx_bad_len++;
			ksz_drop_frame(dev);
			continue;
		}
		len -= KSZ_RX_CRC_LEN;

		/*
		 * Aloca antes de ligar o SDA: a alocação pode dormir, e com o
		 * SDA ligado o chip fica com os registradores inacessíveis.
		 */
		pkt = net_pkt_rx_alloc_with_buffer(ctx->iface, len, NET_AF_UNSPEC, 0,
						   K_MSEC(CONFIG_ETH_KSZ8851SNL_TIMEOUT));
		if (pkt == NULL) {
			LOG_ERR("sem net_pkt para %u bytes", len);
			ctx->stats.rx_no_pkt++;
			ksz_drop_frame(dev);
			continue;
		}

		/* Lê do início do quadro, com auto-incremento do ponteiro */
		ksz_write16(dev, KSZ_RXFDPR, RXFDPR_RXFPAI);
		ksz_fifo_access(dev, true);
		ret = ksz_read_frame(dev, pkt, len);
		ksz_fifo_access(dev, false);

		ksz_drop_frame(dev);

		if (ret < 0) {
			ctx->stats.rx_err++;
			net_pkt_unref(pkt);
			continue;
		}

		ret = net_recv_data(ctx->iface, pkt);
		if (ret < 0) {
			LOG_DBG("pilha de rede recusou o pacote (%d)", ret);
			ctx->stats.rx_reject++;
			ctx->stats.rx_reject_err = ret;
			net_pkt_unref(pkt);
		} else {
			ctx->stats.rx_ok++;
		}
	}
}

/* --- transmissão ---------------------------------------------------------- */

static int eth_ksz8851_tx(const struct device *dev, struct net_pkt *pkt)
{
	const struct eth_ksz8851_config *cfg = dev->config;
	struct eth_ksz8851_data *ctx = dev->data;
	static uint8_t zeros[4]; /* enchimento até múltiplo de 4; spi_buf.buf é void * */
	struct spi_buf *tx_bufs = ctx->bufs;
	struct spi_buf_set tx = {.buffers = tx_bufs};
	uint16_t len = net_pkt_get_len(pkt);
	uint16_t need = ROUND_UP(len + 4, 4);
	size_t pad = ROUND_UP(len, 4) - len;
	struct net_buf *frag;
	uint8_t hdr[KSZ_TX_HDR_LEN];
	uint16_t fid;
	size_t n = 1;
	int64_t deadline;
	int ret;

	if (len > NET_ETH_MAX_FRAME_SIZE) {
		return -EMSGSIZE;
	}

	k_mutex_lock(&ctx->lock, K_FOREVER);

	/* A QMU só aceita o quadro se couber inteiro na TX FIFO */
	deadline = k_uptime_get() + CONFIG_ETH_KSZ8851SNL_TX_TIMEOUT;
	while (ksz_read16(dev, KSZ_TXMIR) < need) {
		if (k_uptime_get() > deadline) {
			LOG_ERR("TX FIFO cheia para %u bytes", len);
			ctx->stats.tx_full++;
			ret = -EAGAIN;
			goto out;
		}
		k_msleep(1);
	}

	fid = ctx->fid++ & TXFR_TXFID_MASK;

	hdr[0] = KSZ_OP_TXFIFO;
	sys_put_le16(fid | TXFR_TXIC, &hdr[1]);
	sys_put_le16(len, &hdr[3]);

	tx_bufs[0].buf = hdr;
	tx_bufs[0].len = sizeof(hdr);

	for (frag = pkt->frags; frag != NULL; frag = frag->frags) {
		if (frag->len == 0) {
			continue;
		}
		if (n >= KSZ_MAX_BUFS + 1) {
			LOG_ERR("quadro de %u bytes em fragmentos demais", len);
			ret = -EMSGSIZE;
			goto out;
		}
		tx_bufs[n].buf = frag->data;
		tx_bufs[n++].len = frag->len;
	}

	if (pad > 0) {
		tx_bufs[n].buf = zeros;
		tx_bufs[n++].len = pad;
	}
	tx.count = n;

	ksz_fifo_access(dev, true);
	ret = spi_write_dt(&cfg->spi, &tx);
	ksz_fifo_access(dev, false);

	if (ret < 0) {
		LOG_ERR("escrita na TXQ falhou (%d)", ret);
		goto out;
	}

	/* Enfileira o quadro para sair */
	ksz_write16(dev, KSZ_TXQCR, TXQCR_METFE);
	ctx->stats.tx_ok++;

out:
	k_mutex_unlock(&ctx->lock);
	return ret;
}

/* --- link ----------------------------------------------------------------- */

static void ksz_check_link(const struct device *dev)
{
	struct eth_ksz8851_data *ctx = dev->data;
	uint16_t p1sr = ksz_read16(dev, KSZ_P1SR);
	bool up = (p1sr & P1SR_LINK_GOOD) != 0;

	if (up == ctx->link_up) {
		return;
	}
	ctx->link_up = up;

	if (up) {
		LOG_INF("link ativo: %sBASE-T %s", (p1sr & P1SR_OP_100M) ? "100" : "10",
			(p1sr & P1SR_OP_FDX) ? "full-duplex" : "half-duplex");
		net_if_carrier_on(ctx->iface);
	} else {
		LOG_INF("link inativo");
		net_if_carrier_off(ctx->iface);
	}
}

/* --- thread de recepção --------------------------------------------------- */

static void ksz_gpio_callback(const struct device *port, struct gpio_callback *cb,
			      gpio_port_pins_t pins)
{
	struct eth_ksz8851_data *ctx = CONTAINER_OF(cb, struct eth_ksz8851_data, gpio_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	k_sem_give(&ctx->int_sem);
}

static void ksz_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct eth_ksz8851_data *ctx = dev->data;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		uint16_t status;

		/*
		 * O timeout é necessário: o INTRN é ativo por nível e o GPIO
		 * do SAM0 só entrega borda, então uma interrupção que chegue
		 * durante o processamento não gera borda nova.
		 */
		k_sem_take(&ctx->int_sem, K_MSEC(CONFIG_ETH_KSZ8851SNL_POLL_MS));

		k_mutex_lock(&ctx->lock, K_FOREVER);

		status = ksz_read16(dev, KSZ_ISR);
		ctx->stats.isr_reg = status;
		ctx->stats.rxcr1 = ksz_read16(dev, KSZ_RXCR1);
		ctx->stats.rxfctr = ksz_read16(dev, KSZ_RXFCTR);

		if (status != 0) {
			ksz_write16(dev, KSZ_ISR, status); /* escrever 1 limpa */
			ctx->stats.isr++;
			ctx->stats.isr_last = status;
		}

		if (status & IRQ_SPIBEI) {
			LOG_ERR("erro no barramento SPI");
			ctx->stats.spi_err++;
		}
		if (status & IRQ_RXOI) {
			LOG_WRN("RX FIFO estourou");
			ctx->stats.rx_ovf++;
		}
		if (status & IRQ_RXI) {
			ctx->stats.rxi++;
			ksz_rx(dev);
		}

		/* Sem depender do IRQ_LCI: cobre o link subir antes de as
		 * interrupções abrirem, ou um LCI se perder.
		 */
		ksz_check_link(dev);

		k_mutex_unlock(&ctx->lock);
	}
}

/* --- API do Zephyr -------------------------------------------------------- */

static void ksz_write_mac(const struct device *dev)
{
	struct eth_ksz8851_data *ctx = dev->data;

	ksz_write16(dev, KSZ_MARH, sys_get_be16(&ctx->mac_addr[0]));
	ksz_write16(dev, KSZ_MARM, sys_get_be16(&ctx->mac_addr[2]));
	ksz_write16(dev, KSZ_MARL, sys_get_be16(&ctx->mac_addr[4]));
}

static enum ethernet_hw_caps eth_ksz8851_get_capabilities(const struct device *dev,
							  struct net_if *iface)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(iface);

	return ETHERNET_LINK_10BASE | ETHERNET_LINK_100BASE;
}

static int eth_ksz8851_set_config(const struct device *dev, struct net_if *iface,
				  enum ethernet_config_type type,
				  const struct ethernet_config *config)
{
	struct eth_ksz8851_data *ctx = dev->data;

	ARG_UNUSED(iface);

	if (type != ETHERNET_CONFIG_TYPE_MAC_ADDRESS) {
		return -ENOTSUP;
	}

	k_mutex_lock(&ctx->lock, K_FOREVER);
	memcpy(ctx->mac_addr, config->mac_address.addr, sizeof(ctx->mac_addr));
	ksz_write_mac(dev);
	k_mutex_unlock(&ctx->lock);

	return 0;
}

/* Põe a QMU em estado conhecido e liga recepção e transmissão */
static void ksz_configure(const struct device *dev)
{
	struct eth_ksz8851_data *ctx = dev->data;

	/* Sai de qualquer modo de economia: sem isto o MAC não grava */
	ksz_write16(dev, KSZ_PMECR,
		    (ksz_read16(dev, KSZ_PMECR) & ~PMECR_PM_MASK) | PMECR_PM_NORMAL);

	/* Reset só da QMU (o reset global já rodou na detecção do chip) */
	ksz_write16(dev, KSZ_GRR, GRR_QMU);
	k_msleep(1);
	ksz_write16(dev, KSZ_GRR, 0);
	k_msleep(1);

	ksz_write_mac(dev);

	/* Transmissão: CRC e enchimento até o mínimo por conta do chip */
	ksz_write16(dev, KSZ_TXCR, TXCR_TXE | TXCR_TXPE | TXCR_TXCRC | TXCR_TXFCE);
	ksz_write16(dev, KSZ_TXFDPR, TXFDPR_TXFPAI);

	ksz_write16(dev, KSZ_RXCR1,
		    RXCR1_RXPAFMA | RXCR1_RXFCE | RXCR1_RXBE | RXCR1_RXUE | RXCR1_RXE);
	/* Quadro inteiro de uma vez, que é como ksz_read_frame lê */
	ksz_write16(dev, KSZ_RXCR2, RXCR2_SRDBL_FRAME);

	/* Interrompe já no primeiro quadro: latência importa mais aqui */
	ksz_write16(dev, KSZ_RXFCTR, 1);
	ksz_write16(dev, KSZ_RXDTTR, 1000);
	ksz_write16(dev, KSZ_RXDBCTR, 4096);

	ctx->rxqcr = RXQCR_RXFCTE | RXQCR_RXDBCTE | RXQCR_RXDTTE;
	ksz_write16(dev, KSZ_RXQCR, ctx->rxqcr);

	ksz_write16(dev, KSZ_P1CR,
		    P1CR_AUTONEGEN | P1CR_ADV_FLOW | P1CR_ADV_100BT_FDX |
			    P1CR_ADV_100BT_HDX | P1CR_ADV_10BT_FDX | P1CR_ADV_10BT_HDX);

	/* Limpa pendências antes de abrir as interrupções */
	ksz_write16(dev, KSZ_ISR, 0xffff);
	ksz_write16(dev, KSZ_IER, IRQ_LCI | IRQ_TXI | IRQ_RXI | IRQ_RXOI | IRQ_SPIBEI);
}

static void eth_ksz8851_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct eth_ksz8851_data *ctx = dev->data;

	net_if_set_link_addr(iface, ctx->mac_addr, sizeof(ctx->mac_addr), NET_LINK_ETHERNET);
	ctx->iface = iface;

	ethernet_init(iface);
	net_if_carrier_off(iface);

	k_thread_create(&ctx->thread, ctx->thread_stack,
			K_KERNEL_STACK_SIZEOF(ctx->thread_stack), ksz_thread, (void *)dev,
			NULL, NULL, K_PRIO_COOP(CONFIG_ETH_KSZ8851SNL_RX_THREAD_PRIO), 0,
			K_NO_WAIT);
	k_thread_name_set(&ctx->thread, "ksz8851");

	/* O link pode já estar de pé antes da primeira interrupção */
	k_mutex_lock(&ctx->lock, K_FOREVER);
	ksz_check_link(dev);
	k_mutex_unlock(&ctx->lock);
}

static const struct ethernet_api eth_ksz8851_api = {
	.iface_api.init = eth_ksz8851_iface_init,
	.get_capabilities = eth_ksz8851_get_capabilities,
	.set_config = eth_ksz8851_set_config,
	.send = eth_ksz8851_tx,
};

static int eth_ksz8851_init(const struct device *dev)
{
	const struct eth_ksz8851_config *cfg = dev->config;
	struct eth_ksz8851_data *ctx = dev->data;
	uint16_t cider;
	int ret;

	k_mutex_init(&ctx->lock);
	k_sem_init(&ctx->int_sem, 0, 1);

	if (!spi_is_ready_dt(&cfg->spi)) {
		LOG_ERR("SPI %s não está pronto", cfg->spi.bus->name);
		return -ENODEV;
	}

	if (cfg->reset.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->reset)) {
			LOG_ERR("GPIO de reset não está pronto");
			return -ENODEV;
		}
		/*
		 * Dreno aberto feito à mão (o GPIO do SAM0 recusa
		 * GPIO_SINGLE_ENDED): ao soltar, o pino vira entrada com
		 * pull-up em vez de ficar em nível alto. Necessário porque no
		 * EXT3 da SAMD21 Xpro o nRST cai no SW0, que aterra o pino
		 * quando apertado. Os 10 ms são exigência do datasheet.
		 */
		gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_ACTIVE);
		k_msleep(10);
		gpio_pin_configure_dt(&cfg->reset, GPIO_INPUT | GPIO_PULL_UP);
		k_msleep(10);
	}

	if (!gpio_is_ready_dt(&cfg->interrupt)) {
		LOG_ERR("GPIO de interrupção não está pronto");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->interrupt, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("não configurou o pino de interrupção (%d)", ret);
		return ret;
	}

	gpio_init_callback(&ctx->gpio_cb, ksz_gpio_callback, BIT(cfg->interrupt.pin));
	ret = gpio_add_callback(cfg->interrupt.port, &ctx->gpio_cb);
	if (ret < 0) {
		return ret;
	}

	/* Reset global por software antes de olhar o ID */
	ksz_write16(dev, KSZ_GRR, GRR_GSR);
	k_msleep(1);
	ksz_write16(dev, KSZ_GRR, 0);
	k_msleep(1);

	cider = ksz_read16(dev, KSZ_CIDER);
	ctx->stats.cider = cider; /* guardado mesmo em caso de falha, para depuração */
	if ((cider & ~CIDER_REV_MASK) != CIDER_ID) {
		/*
		 * 0x7088 em vez de 0x8870 é byte order trocado; 0x0000 ou
		 * 0xffff costuma ser fiação, CS ou alimentação.
		 */
		LOG_ERR("KSZ8851SNL não encontrado: CIDER = 0x%04x (esperado 0x%04x)", cider,
			CIDER_ID);
		return -ENODEV;
	}

	if (cfg->random_mac) {
		sys_rand_get(ctx->mac_addr, sizeof(ctx->mac_addr));
		ctx->mac_addr[0] = (ctx->mac_addr[0] & 0xfe) | 0x02; /* local, unicast */
	}

	ksz_configure(dev);

	ret = gpio_pin_interrupt_configure_dt(&cfg->interrupt, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("não habilitou a interrupção (%d)", ret);
		return ret;
	}

	LOG_INF("KSZ8851SNL rev %u, MAC %02x:%02x:%02x:%02x:%02x:%02x",
		(cider & CIDER_REV_MASK) >> CIDER_REV_SHIFT, ctx->mac_addr[0],
		ctx->mac_addr[1], ctx->mac_addr[2], ctx->mac_addr[3], ctx->mac_addr[4],
		ctx->mac_addr[5]);

	return 0;
}

#define KSZ8851SNL_DEFINE(inst)                                                                    \
	static struct eth_ksz8851_data eth_ksz8851_data_##inst = {                                 \
		.mac_addr = DT_INST_PROP_OR(inst, local_mac_address, {0}),                         \
	};                                                                                         \
                                                                                                   \
	static const struct eth_ksz8851_config eth_ksz8851_config_##inst = {                       \
		.spi = SPI_DT_SPEC_INST_GET(inst, SPI_WORD_SET(8) | SPI_TRANSFER_MSB),             \
		.interrupt = GPIO_DT_SPEC_INST_GET(inst, int_gpios),                               \
		.reset = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),                         \
		.random_mac = DT_INST_PROP(inst, zephyr_random_mac_address),                       \
	};                                                                                         \
                                                                                                   \
	ETH_NET_DEVICE_DT_INST_DEFINE(inst, eth_ksz8851_init, NULL, &eth_ksz8851_data_##inst,      \
				      &eth_ksz8851_config_##inst, CONFIG_ETH_INIT_PRIORITY,        \
				      &eth_ksz8851_api, NET_ETH_MTU);

DT_INST_FOREACH_STATUS_OKAY(KSZ8851SNL_DEFINE)
