/*
 * Servidor HTTP mínimo sobre sockets: uma conexão por vez, uma requisição por
 * conexão (Connection: close). O servidor HTTP do Zephyr não cabe folgado nos
 * 32 KB de RAM do SAMD21 junto com a rede e o SD.
 */

#include "api_http.h"
#include "armazenamento.h"
#include "tabela.h"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * O zsock_send() transmite nesta própria thread, descendo pelo TCP até o
 * driver SPI da Ethernet; com 1536 B a pilha estourava na primeira resposta.
 */
#define TAM_PILHA 2048
#define PRIORIDADE 7
#define TIMEOUT_RECV_S 2

K_THREAD_STACK_DEFINE(pilha_http, TAM_PILHA);
static struct k_thread thread_http;

/* Só a primeira linha importa ("GET /health HTTP/1.1"); o resto é descartado */
static char req[128];
static char cabecalho[160];
/* Corpo de qualquer resposta; o maior é o /data com 4 nós (~450 B) */
static char corpo[512];

/* Lê a requisição até a linha em branco que fecha os cabeçalhos */
static size_t ler_requisicao(int sock)
{
	char bloco[64];
	size_t usado = 0;
	uint32_t ultimos = 0;

	for (;;) {
		ssize_t n = zsock_recv(sock, bloco, sizeof(bloco), 0);

		if (n <= 0) {
			break;
		}
		for (ssize_t i = 0; i < n; i++) {
			if (usado < sizeof(req) - 1) {
				req[usado++] = bloco[i];
			}
			ultimos = (ultimos << 8) | (uint8_t)bloco[i];
		}
		/*
		 * Ler até o fim evita fechar com dados pendentes, o que faria o
		 * TCP mandar RST e o cliente perder a resposta.
		 */
		if (ultimos == 0x0d0a0d0a) { /* "\r\n\r\n" */
			break;
		}
	}

	req[usado] = '\0';
	return usado;
}

static void enviar(int sock, const char *buf, size_t tam)
{
	while (tam > 0) {
		ssize_t n = zsock_send(sock, buf, tam, 0);

		if (n <= 0) {
			return;
		}
		buf += n;
		tam -= n;
	}
}

static void responder(int sock, int codigo, const char *motivo, const char *texto)
{
	size_t tam_corpo = strlen(texto);
	int n = snprintf(cabecalho, sizeof(cabecalho),
			 "HTTP/1.1 %d %s\r\n"
			 "Content-Type: application/json\r\n"
			 "Content-Length: %u\r\n"
			 "Connection: close\r\n\r\n",
			 codigo, motivo, (uint32_t)tam_corpo);

	enviar(sock, cabecalho, n);
	enviar(sock, texto, tam_corpo);
}

/* A requisição é "<metodo> <caminho>[?query] HTTP/1.1"? */
static bool rota(const char *metodo, const char *caminho)
{
	size_t lm = strlen(metodo);
	size_t lc = strlen(caminho);
	char fim;

	if (strncmp(req, metodo, lm) != 0 || req[lm] != ' ' ||
	    strncmp(req + lm + 1, caminho, lc) != 0) {
		return false;
	}
	fim = req[lm + 1 + lc];
	return fim == ' ' || fim == '?';
}

static int get_health(int sock)
{
	/* "degraded": no ar, mas sem SD as leituras não estão sendo gravadas */
	snprintf(corpo, sizeof(corpo),
		 "{\"status\":\"%s\",\"uptime_ms\":%u,\"nodes_online\":%d}",
		 armazenamento_ok() ? "ok" : "degraded",
		 (uint32_t)k_uptime_get(), tabela_nos_online());
	responder(sock, 200, "OK", corpo);
	return 200;
}

static int get_data(int sock)
{
	if (tabela_json(corpo, sizeof(corpo)) < 0) {
		responder(sock, 500, "Internal Server Error",
			  "{\"erro\":\"resposta maior que o buffer\"}");
		return 500;
	}
	responder(sock, 200, "OK", corpo);
	return 200;
}

/* Retorna true se a requisição pediu para reiniciar a placa */
static bool atender(int sock)
{
	bool reiniciar = false;
	int codigo;

	if (ler_requisicao(sock) == 0) {
		return false;
	}

	if (rota("GET", "/health")) {
		codigo = get_health(sock);
	} else if (rota("GET", "/data")) {
		codigo = get_data(sock);
	} else if (rota("POST", "/reset")) {
		/* Responde antes: o supervisor precisa saber que o pedido chegou */
		codigo = 200;
		responder(sock, 200, "OK", "{\"status\":\"reiniciando\"}");
		reiniciar = true;
	} else {
		codigo = 404;
		responder(sock, 404, "Not Found", "{\"erro\":\"rota desconhecida\"}");
	}

	/* Mostra só a primeira linha da requisição */
	printk("HTTP: %.*s -> %d\r\n", (int)strcspn(req, "\r\n"), req, codigo);
	return reiniciar;
}

static void reiniciar_placa(void)
{
	printk("HTTP: reset pedido, reiniciando\r\n");

	/* Não deixa o reboot cortar uma linha do CSV no meio */
	armazenamento_finalizar();

	/* Tempo para o TCP entregar a resposta e o FIN ao supervisor */
	k_sleep(K_MSEC(200));
	sys_reboot(SYS_REBOOT_COLD);
}

static void servidor_http(void *p1, void *p2, void *p3)
{
	struct net_sockaddr_in addr = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(CONFIG_SERVIDOR_HTTP_PORTA),
		/* sin_addr zerado = qualquer IP da placa */
	};
	struct zsock_timeval tv = { .tv_sec = TIMEOUT_RECV_S };
	int escuta;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	escuta = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
	if (escuta < 0) {
		printk("HTTP: falha ao criar o socket (%d)\r\n", -errno);
		return;
	}

	if (zsock_bind(escuta, (struct net_sockaddr *)&addr, sizeof(addr)) < 0 ||
	    zsock_listen(escuta, 2) < 0) {
		printk("HTTP: falha ao abrir a porta %d (%d)\r\n",
		       CONFIG_SERVIDOR_HTTP_PORTA, -errno);
		zsock_close(escuta);
		return;
	}

	printk("HTTP: escutando na porta %d\r\n", CONFIG_SERVIDOR_HTTP_PORTA);

	for (;;) {
		int cliente = zsock_accept(escuta, NULL, NULL);
		bool reiniciar;

		if (cliente < 0) {
			k_sleep(K_MSEC(100));
			continue;
		}

		/* Cliente que conecta e não manda nada não trava o servidor */
		zsock_setsockopt(cliente, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO,
				 &tv, sizeof(tv));
		reiniciar = atender(cliente);
		zsock_close(cliente);

		if (reiniciar) {
			reiniciar_placa();
		}
	}
}

void api_http_iniciar(void)
{
	k_thread_create(&thread_http, pilha_http, K_THREAD_STACK_SIZEOF(pilha_http),
			servidor_http, NULL, NULL, NULL, PRIORIDADE, 0, K_NO_WAIT);
	k_thread_name_set(&thread_http, "http");
}

