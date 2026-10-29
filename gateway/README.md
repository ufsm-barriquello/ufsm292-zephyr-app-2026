# Gateway radio transport

## Recebendo pacotes do sensor

O gateway usa um socket `AF_PACKET` /`SOCK_RAW`.

Recebe frames MAC sem FCS:

```c
/* sock is opened once with radio_socket_open(&sock, radio_iface). */
int len = sensor_frame_encode(psdu, sizeof(psdu), &cfg, &reading);
if (len > 0) {
        len = net_send_raw(&sock, psdu, len);
}

len = radio_socket_recv(&sock, psdu, sizeof(psdu), 0);
if (len > 0) {
        int ret = sensor_frame_decode(psdu, len, &reading);
        /* Handle ret and reading. */
}
```

O receptor existe em `gateway/src/radio_socket.{c,h}`. Chamados retornam
contagem de bytes ou errno negativo; setup retorna zero ou errno negativo.

## Validação em simulação

```sh
west build -b samr21_xpro gateway -d build-gateway-socket
west build -b native_sim/native/64 tests/net/radio_socket \
  -d build-radio-socket-test -- -DZEPHYR_TOOLCHAIN_VARIANT=host
set -o pipefail
build-radio-socket-test/zephyr/zephyr.exe 2>&1 | tee /tmp/ufsm292_socket_local_01.log
```


## Threads do gateway

`main()` configura o radio/socket, depois inicia o loop do HTTP. 
Falhas no setup do rádio não bloqueiam o inicio do HTTP.
`radio_rx_id` uses a 2048-byte stack at priority 5 and waits on `rx_ready`
until radio setup succeeds. There is no separate gateway or HTTP thread.
`radio_rx_id` usa pilha de 2048 bytes e testa `rx_ready`até que o radio esteja funcionando.


O setup do ethernet (`eth_setup()`) corre durante o inicio e configura DHCP e
callbacks para net-mgmt e eventos do enterço IPV4.


Teste no simulador os mecanismos de recuperação de falha do ethernet.

```sh
python3 tests/drivers/ethernet/ksz8851snl/run.py
```

O loop RX recebe um frame, decodifica `sensor_reading`, e chama
`handle_sensor_reading()`. Esse handler atualize a tabela usando
`gateway_memory_lock`, e depois imprime para o serial. Os endpoints HTTP copiam
o estado também com o lock, soltando antes de encodificar o  JSON encoding e enviar para a rede.
Isso permite que o gateway continue recebendo durante a transmissão HTTP.


## RAM

A tabela de memória segura as ultimas leituras para até quatro nós com IDs
distintos, usando 128 bytes para cada nó. Se a tabela já está cheia rejeita
com `-ENOSPC`.

O laço HTTP usa um buffer de JSON de 515 bytes e um chunk de entrade de 128 bytes.
O driver KZS8851 usa um buffer de 1528 bytes, rejeitando jumbo frames.

`NET_MAX_CONTEXTS=4` cobre o rádio, HTTP, cliente e um extra cliente extra que recebe
HTTP 500 (sobrecarregado).
`NET_MAX_CONN=5` permite uso de DHCP junto com esses outros clientes.

O projeto usa 32244/32768 bytes de RAM (98.40%) e 127280/262144 bytes de flash (48.55%).


## API HTTP

`gateway/src/http_server.c` serve um cliente de cada vez usando o parser HTTP do 
Zephyr. Ele escuta em `CONFIG_GATEWAY_HTTP_PORT` (= 80) e responde com `Content-Length`, 
`application/json`, e `Connection: close`

- `GET /sensors`: 200 com array JSON das leituras.
- `GET /health`: 200 com `status`, 64-bit `uptime_ms`, e `online_nodes`.
  `status` é `ok`  quando radio está functionando, `degraded` caso contrário; nós
  online são contados se transmitiram nos ultimos `CONFIG_GATEWAY_NODE_ONLINE_TIMEOUT_MS`, (= 30000 ms).
- `POST /reset`: 202 com  `{"status":"restarting"}`. Após isso reseta.

Outros métodos em uma dessas rotas retorna 405 with `Allow`; desconhecidos
retornam 404.  Pedidos malformados retornam 400; Pedidos acima de 2048 bytes retornam 413.

Cada pedido é logado para o serial.

Não possui autenticação, portando só deve ser usado em rede confiavel. 
Permitindo que qualquer um resete o gateway pode impactar a disponibilidade do sistema.

```sh
curl -i http://GATEWAY_IP/sensors
curl -i http://GATEWAY_IP/health
curl -i -X POST http://GATEWAY_IP/reset
```


