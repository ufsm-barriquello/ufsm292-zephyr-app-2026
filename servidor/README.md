# Servidor (grupo C): datalogger

Subsistema **C** do projeto final de UFSM00292 (Projeto de Sistemas Embarcados,
2026/2).

O servidor busca periodicamente as leituras dos nós sensores no gateway, grava
cada leitura nova num CSV no cartão microSD com data e hora reais e responde ao
supervisor por HTTP (estado, últimas leituras e reset).

```
Nó sensor (A) ──rádio──► Gateway (B) ──Ethernet──► SERVIDOR (C) ◄──Ethernet── Supervisor (D)
                         GET /sensors              busca, grava    GET /health, GET /data,
                                                   no SD, responde POST /reset
```

## Hardware

| Placa | Conector | Barramento | Uso |
|---|---|---|---|
| SAM D21 Xplained Pro (Cortex-M0+, 256 KB flash, 32 KB RAM) | | | placa principal |
| Ethernet1 Xplained Pro (KSZ8851SNL) | **EXT3** | SPI no SERCOM5 | rede |
| IO1 Xplained Pro (microSD) | **EXT1** | SPI no SERCOM0 | cartão SD |

Os pinos estão em [boards/samd21_xpro.overlay](boards/samd21_xpro.overlay):

| Sinal | Ethernet1 (EXT3) | IO1 / SD (EXT1) |
|---|---|---|
| CS | PB17 | PA05 |
| MOSI | PB22 | PA06 |
| MISO | PB16 | PA04 |
| SCK | PB23 | PA07 |
| Interrupção | PA28 | |
| Reset | PA15 | |

## Organização

```
servidor/
├── CMakeLists.txt
├── Kconfig              ← opções do servidor (portas, IP do gateway, períodos)
├── prj.conf             ← configuração do Zephyr (rede, SD, pilhas)
├── boards/
│   └── samd21_xpro.overlay
├── src/
│   ├── main.c           ← inicialização e laço principal (relógio + coletor)
│   ├── leitura.h        ← struct leitura: formato comum a todos os módulos
│   ├── rede.c/.h        ← espera o IPv4 do DHCP
│   ├── relogio.c/.h     ← hora real via SNTP (NTP.br)
│   ├── http_cliente.c/.h← cliente HTTP mínimo (usado pelo coletor)
│   ├── coletor.c/.h     ← GET /sensors no gateway e leitura do JSON
│   ├── tabela.c/.h      ← última leitura de cada nó, em RAM
│   ├── armazenamento.c/.h ← CSV no SD
│   └── api_http.c/.h    ← servidor HTTP: /health, /data, /reset
└── testes/              ← testes isolados de hardware
    ├── pisca-led/       ← confere build, gravação e serial
    ├── rede/            ← só a Ethernet: DHCP + ping
    └── sd/              ← só o SD: grava e lê de volta um CSV
```

## Fluxo

### Inicialização ([main.c](src/main.c))

1. **Rede:** registra o aviso de IP recebido. O DHCP começa sozinho quando o
   cabo é ligado.
2. **SD:** monta o cartão e cria o `LEITURAS.CSV` com cabeçalho se ele ainda
   não existir. Sem SD, o servidor continua no ar e o `/health` informa
   `"degraded"`.
3. **HTTP:** cria a thread que escuta na porta 5501. Ela começa antes de existir
   IP e passa a atender quando o IP chega.
4. **Espera o IP do DHCP**, avisando a cada 10 s enquanto não chega.
5. **Laço principal**, para sempre:
   - **relógio:** sincroniza com o NTP na primeira volta e depois de hora em
     hora. Sem internet, tenta de novo a cada minuto;
   - **coletor:** consulta o gateway;
   - espera `CONFIG_SERVIDOR_COLETA_MS` (5 s) e repete.

### Threads

| Thread | O que faz |
|---|---|
| `main` | relógio e coletor: busca no gateway e grava no SD |
| `http` | atende o supervisor, uma conexão por vez |
| `rx_q`, `tcp`, `sysworkq` | do Zephyr: recebem e enviam os pacotes de rede |

Dois mutex protegem o que é compartilhado entre threads: um para o **SD** (o
coletor grava e o reset desmonta) e outro para a **tabela** (o coletor escreve
e o HTTP lê).

### Do gateway ao supervisor

```
a cada ~5 s (main):
  GET /sensors no gateway
     └─ para cada nó com leitura nova (last_seen_ms mudou):
           ├─ atualiza a tabela em RAM  → o /data já mostra
           └─ grava 1 linha no SD

a qualquer momento (thread http):
  GET /data   → responde com a tabela
  GET /health → status, uptime e nós online
  POST /reset → desmonta o SD e reinicia a placa
```

## Coletor ([coletor.c](src/coletor.c))

A cada `CONFIG_SERVIDOR_COLETA_MS` (5 s), depois de terminada a consulta
anterior:

1. **Requisição:** `GET http://<gateway>:<porta>/sensors`, com 1,5 s de limite
   para conectar e 2 s para receber.
2. **JSON:** lido com a biblioteca de JSON do Zephyr, até 4 nós (o mesmo limite
   do gateway). A temperatura `"24.53"` vira o inteiro `2453`, em centésimos,
   sem usar float.
3. **Filtro de repetição:** o gateway devolve sempre a última leitura de cada
   nó, mesmo que o nó não tenha mandado nada novo. O servidor compara o
   `last_seen_ms` com o da tabela:
   - mudou: leitura nova, atualiza a tabela e grava no SD;
   - não mudou: mesma leitura, descarta.

Formato esperado do gateway (o mesmo de
[lib/gateway_json/sensors_json.c](../lib/gateway_json/sensors_json.c)):

```json
[{"node_id":1,"light":320,"temp_c":24.53,"accel":[10,-20,980],"last_seen_ms":1234}]
```

Se um nó mandar leituras mais rápido que o período de coleta, as intermediárias
se perdem, porque o gateway só guarda a última de cada nó.

## Gravação no SD ([armazenamento.c](src/armazenamento.c))

Arquivo `LEITURAS.CSV` na raiz do cartão, uma linha por leitura nova:

```csv
data_hora,recebido_ms,node_id,light,temp_c,accel_x,accel_y,accel_z,last_seen_ms
2026-10-22 17:01:36,152340,1,1291,21.50,0,0,0,2317455
```

| Coluna | Origem |
|---|---|
| `data_hora` | hora real do NTP, horário de Brasília. Vazia se o NTP ainda não respondeu. |
| `recebido_ms` | uptime do servidor na gravação: reserva quando falta a `data_hora` |
| `node_id`, `light`, `temp_c`, `accel_*` | vêm do gateway |
| `last_seen_ms` | uptime do **gateway** quando ele recebeu a leitura do nó |

Cada linha **abre o arquivo em modo append, escreve e fecha**. Isso custa uns
15 ms, mas se a placa reiniciar ou perder energia, no máximo a linha em andamento
se perde. No boot, se o arquivo já existe, o servidor continua do fim: o
histórico nunca é apagado.

O `/data` **não lê o SD**: ele responde da tabela em RAM, que guarda só a última
leitura de cada nó e zera quando a placa reinicia. O SD é o histórico
permanente, para ser analisado depois (por exemplo, abrindo o CSV no PC).

## Rotas HTTP ([api_http.c](src/api_http.c))

Porta **5501** (`CONFIG_SERVIDOR_HTTP_PORTA`). Uma conexão por vez, uma
requisição por conexão. Toda resposta é JSON, com `Content-Length` e
`Connection: close`.

### `GET /health`

Para o supervisor saber se o servidor está vivo.

```json
{"status":"ok","uptime_ms":45210,"nodes_online":2}
```

| Campo | Significado |
|---|---|
| `status` | `"ok"`, ou `"degraded"` se o SD não montou (no ar, mas sem gravar) |
| `uptime_ms` | tempo desde o boot. Se cair, a placa reiniciou. |
| `nodes_online` | nós com leitura nova nos últimos `CONFIG_SERVIDOR_NO_ONLINE_MS` (30 s) |

### `GET /data`

Últimas leituras de cada nó, no mesmo formato do `/sensors` do gateway. Vem da
tabela em RAM, então fica até um período de coleta atrás do gateway. Retorna
`[]` enquanto nenhuma leitura chegou, por exemplo logo depois do boot.

```json
[{"node_id":1,"light":1291,"temp_c":21.50,"accel":[0,0,0],"last_seen_ms":2317455}]
```

### `POST /reset`

Recuperação de falhas pelo supervisor.

```json
{"status":"reiniciando"}
```

O servidor responde primeiro, espera qualquer gravação em andamento terminar,
desmonta o SD, aguarda 200 ms para a resposta chegar e reinicia. O histórico no
SD é mantido.

Só aceita **POST**. Abrir `/reset` no navegador (que faz GET) responde 404.

### Outras rotas

```json
{"erro":"rota desconhecida"}
```

com código **404**.

## Configuração

As opções do servidor ficam no [Kconfig](Kconfig) e são compiladas no firmware:
depois de mudar alguma, é preciso compilar e gravar de novo.

| Opção | Padrão | Significado |
|---|---|---|
| `CONFIG_SERVIDOR_HTTP_PORTA` | `5501` | porta das rotas para o supervisor |
| `CONFIG_SERVIDOR_GATEWAY_ADDR` | `"192.168.100.20"` | IPv4 do gateway (valor de exemplo: trocar na integração) |
| `CONFIG_SERVIDOR_GATEWAY_PORTA` | `80` | porta do gateway |
| `CONFIG_SERVIDOR_COLETA_MS` | `5000` | período de consulta ao gateway |
| `CONFIG_SERVIDOR_NO_ONLINE_MS` | `30000` | janela do `nodes_online` |

Para apontar para o gateway, descomente e ajuste no [prj.conf](prj.conf):

```
CONFIG_SERVIDOR_GATEWAY_ADDR="<ip-do-gateway>"
```

O IP do gateway aparece na serial dele, na linha do DHCP. Antes de gravar,
confira se ele responde: abra `http://<ip-do-gateway>/sensors` no navegador.

## Compilar e gravar

Na raiz do repositório:

```bash
west build -d build/servidor -b samd21_xpro servidor -p always
west flash -d build/servidor
```

No Windows, com o Python da Microsoft Store, use `python -m west` no lugar de
`west`.

Para gravar um firmware compilado em outro lugar (por exemplo, no Codespaces),
baixe o `build/servidor/zephyr/zephyr.bin` e use:

```bash
edbg -bpv -t samd21 -f zephyr.bin
```

Serial (EDBG Virtual COM Port): **115200 baud**.

## Saída esperada na serial

```text
Servidor (grupo C)
SD: /SD:/LEITURAS.CSV com 1624 bytes, acrescentando
Rede: aguardando DHCP...
HTTP: escutando na porta 5501
Rede: IP 192.168.253.135
Coletor: gateway em 192.168.253.146:80, a cada 5000 ms
Relogio: 2026-10-22 16:59:16 (horario de Brasilia)
SD: leitura nova do no 1 gravada 2026-10-22 16:59:17
HTTP: GET /data HTTP/1.1 -> 200
```

## Como testar

Troque `<ip>` pelo IP que aparece na serial (`Rede: IP ...`). O computador
precisa estar na mesma rede da placa.

```bash
curl http://<ip>:5501/health
curl http://<ip>:5501/data
curl -X POST http://<ip>:5501/reset
```

No PowerShell, use `curl.exe`: o `curl` sozinho é outro comando.

| Teste | Esperado |
|---|---|
| `/health` duas vezes | `"status":"ok"` e `uptime_ms` maior na segunda |
| `/data` sem gateway | `[]` |
| `/data` com gateway | os mesmos nós do `/sensors` do gateway. Para comparar, use o `last_seen_ms`: se for igual, os valores têm que ser iguais. |
| `POST /reset` | `{"status":"reiniciando"}` e a placa reinicia |
| sem cartão SD | `"status":"degraded"` |
| CSV | linhas novas com `data_hora`, sem repetição |

### Testes isolados de hardware

Para separar problema de hardware de problema de código:

```bash
west build -d build/servidor-pisca -b samd21_xpro servidor/testes/pisca-led -p always
west build -d build/servidor-rede  -b samd21_xpro servidor/testes/rede      -p always
west build -d build/servidor-sd    -b samd21_xpro servidor/testes/sd        -p always
```

| Teste | Esperado |
|---|---|
| `pisca-led` | LED0 piscando a cada 500 ms e `LED ligado`/`LED desligado` na serial |
| `rede` | `IP obtido por DHCP: ...` a cada 5 s e resposta ao `ping` |
| `sd` | 10 linhas gravadas e lidas de volta no `LEITURAS.CSV` |

## Limitações e cuidados

- **RAM no limite:** o firmware usa cerca de 92% dos 32 KB. Ao acrescentar
  código, confira a linha `RAM:` do build. Estouros de pilha aparecem na serial
  como `Stack overflow` com o nome da thread (`CONFIG_THREAD_NAME`).
- **Gateway inalcançável:** com o IP do gateway apontando para um endereço que
  não existe na rede, o coletor tenta a cada 5 s e ocupa os poucos buffers de
  rede. Já se viu as rotas HTTP deixarem de responder (o cliente desiste depois
  de 21 s) e o aviso `rx drop` na serial. Configure o IP do gateway só quando
  ele estiver ligado na mesma rede.
- **IP por DHCP:** o IP muda com a rede. Confira na serial antes de passar ao supervisor ou reserve o IP
  pelo MAC no roteador.
- **NTP** precisa de acesso à internet (UDP 123). Sem ele, a `data_hora` fica
  vazia e o `recebido_ms` continua sendo gravado.
- **`last_seen_ms`:** o gateway manda o seu próprio uptime, e o supervisor lê o
  campo como idade da leitura. O `/data` repassa o valor do gateway sem mudar.
