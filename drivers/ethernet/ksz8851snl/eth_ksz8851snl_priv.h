/*
 * Registradores do Microchip KSZ8851SNL (ATETHERNET1-XPRO), conferidos contra
 * o datasheet e contra o ks8851.h do Linux.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ETH_KSZ8851SNL_PRIV_H_
#define ETH_KSZ8851SNL_PRIV_H_

#include <zephyr/sys/util.h>

/*
 * Comando SPI: 16 bits, MSB primeiro.
 *
 *   bits 15:14  opcode
 *   bits 13:10  byte enable (o barramento interno do chip é de 32 bits)
 *   bits  9:2   endereço A7:A0
 *   bits  1:0   ignorados
 *
 * Acesso à FIFO (RXQ/TXQ) usa só o primeiro byte: o opcode sozinho.
 */
#define KSZ_OP_RD	0x00
#define KSZ_OP_WR	0x40
#define KSZ_OP_RXFIFO	0x80
#define KSZ_OP_TXFIFO	0xc0

/* Byte enable de um acesso de 16 bits: bytes 2-3 em endereço ímpar em words */
#define KSZ_BE(reg)	(((reg) & 2) ? 0x0c : 0x03)

#define KSZ_CMD0(op, reg) ((op) | (KSZ_BE(reg) << 2) | ((reg) >> 6))
#define KSZ_CMD1(reg)	  (((reg) & 0x3f) << 2)

/* Chip Configuration */
#define KSZ_CCR			0x08
#define CCR_EEPROM		BIT(9)

/* MAC: MARH tem os dois primeiros octetos, MARL os dois últimos; em cada
 * registrador o byte alto é o octeto de menor índice.
 */
#define KSZ_MARL		0x10
#define KSZ_MARM		0x12
#define KSZ_MARH		0x14

/* Global Reset */
#define KSZ_GRR			0x26
#define GRR_QMU			BIT(1)
#define GRR_GSR			BIT(0)

/* Transmit Control */
#define KSZ_TXCR		0x70
#define TXCR_FTXQ		BIT(4)
#define TXCR_TXFCE		BIT(3)
#define TXCR_TXPE		BIT(2)
#define TXCR_TXCRC		BIT(1)
#define TXCR_TXE		BIT(0)

/* Receive Control 1 / 2 */
#define KSZ_RXCR1		0x74
#define RXCR1_FRXQ		BIT(15)
#define RXCR1_RXPAFMA		BIT(11)
#define RXCR1_RXFCE		BIT(10)
#define RXCR1_RXBE		BIT(7)
#define RXCR1_RXME		BIT(6)
#define RXCR1_RXUE		BIT(5)
#define RXCR1_RXAE		BIT(4)
#define RXCR1_RXE		BIT(0)

#define KSZ_RXCR2		0x76
#define RXCR2_SRDBL_FRAME	(0x4 << 5)
#define RXCR2_IUFFP		BIT(4)
#define RXCR2_RXIUFCEZ		BIT(3)

/* TX Memory Information: bytes livres na TX FIFO */
#define KSZ_TXMIR		0x78

/* RX Frame Header Status / Byte Count */
#define KSZ_RXFHSR		0x7c
#define RXFHSR_RXFV		BIT(15)
#define RXFHSR_RXMR		BIT(4)
#define RXFHSR_RXFTL		BIT(2)
#define RXFHSR_RXRF		BIT(1)
#define RXFHSR_RXCE		BIT(0)
#define RXFHSR_ERR		(RXFHSR_RXMR | RXFHSR_RXFTL | RXFHSR_RXRF | RXFHSR_RXCE)

#define KSZ_RXFHBCR		0x7e
#define RXFHBCR_CNT_MASK	0x0fff

/* TXQ / RXQ Command */
#define KSZ_TXQCR		0x80
#define TXQCR_METFE		BIT(0)

#define KSZ_RXQCR		0x82
#define RXQCR_RXDTTE		BIT(7)
#define RXQCR_RXDBCTE		BIT(6)
#define RXQCR_RXFCTE		BIT(5)
#define RXQCR_ADRFE		BIT(4)
#define RXQCR_SDA		BIT(3)
#define RXQCR_RRXEF		BIT(0)

/* Ponteiros das FIFOs */
#define KSZ_TXFDPR		0x84
#define TXFDPR_TXFPAI		BIT(14)
#define KSZ_RXFDPR		0x86
#define RXFDPR_RXFPAI		BIT(14)

/* Gatilhos de interrupção de recepção */
#define KSZ_RXDTTR		0x8c
#define KSZ_RXDBCTR		0x8e

/* Interrupt Enable / Status */
#define KSZ_IER			0x90
#define KSZ_ISR			0x92
#define IRQ_LCI			BIT(15)	/* link mudou */
#define IRQ_TXI			BIT(14)	/* quadro transmitido */
#define IRQ_RXI			BIT(13)	/* quadro recebido */
#define IRQ_RXOI		BIT(11)	/* RX FIFO estourou */
#define IRQ_SPIBEI		BIT(1)	/* erro no barramento SPI */

/* RX Frame Count & Threshold: contagem de quadros no byte alto */
#define KSZ_RXFCTR		0x9c
#define RXFCTR_RXFC_SHIFT	8

/* Chip ID and Enable */
#define KSZ_CIDER		0xc0
#define CIDER_ID		0x8870
#define CIDER_REV_MASK		(0x7 << 1)
#define CIDER_REV_SHIFT		1

/* Power Management Event Control */
#define KSZ_PMECR		0xd4
#define PMECR_PM_MASK		0x3
#define PMECR_PM_NORMAL		0x0

/* PHY da porta 1 */
#define KSZ_P1CR		0xf6
#define P1CR_AUTONEGEN		BIT(7)
#define P1CR_ADV_FLOW		BIT(4)
#define P1CR_ADV_100BT_FDX	BIT(3)
#define P1CR_ADV_100BT_HDX	BIT(2)
#define P1CR_ADV_10BT_FDX	BIT(1)
#define P1CR_ADV_10BT_HDX	BIT(0)

#define KSZ_P1SR		0xf8
#define P1SR_OP_100M		BIT(10)
#define P1SR_OP_FDX		BIT(9)
#define P1SR_LINK_GOOD		BIT(5)

/* Palavra de controle do cabeçalho de transmissão */
#define TXFR_TXIC		BIT(15)
#define TXFR_TXFID_MASK		0x003f

/*
 * Antes do quadro, a leitura da RXQ entrega 4 bytes de status e 4 de lixo.
 * O quadro em si vem com o CRC de 4 bytes no fim, que é descartado.
 */
#define KSZ_RX_HDR_LEN		8
#define KSZ_RX_CRC_LEN		4

/* 1 byte de opcode + 2 do frame ID + 2 do comprimento */
#define KSZ_TX_HDR_LEN		5

#endif /* ETH_KSZ8851SNL_PRIV_H_ */
