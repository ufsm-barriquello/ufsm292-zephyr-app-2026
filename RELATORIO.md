# Relatório final — Grupo D: Supervisor

**Projeto Final — Sistema de Sensoriamento Sem Fio**
Projeto de Sistemas Embarcados — Engenharia de Computação — UFSM — 2026/2
Professor: Carlos Henrique Barriquello

**Grupo D (Supervisor):** Arthur Portella, Luis Pedro Davila Zorzi e Claus Lorenzo Braccini da Silva

Apresentação: vídeo entregue junto com este relatório.

---

## 1. Objetivo

O sistema da turma tem quatro subsistemas, um por grupo:

- **nó sensor** (grupo A): mede luz, temperatura e aceleração e transmite por rádio 802.15.4;
- **gateway** (grupo B): faz a ponte entre o rádio e a Ethernet;
- **servidor** (grupo C): é o datalogger;
- **supervisor** (grupo D, este trabalho).

O supervisor roda numa **SAM D21 Xplained Pro** com **OLED1 Xplained Pro** e
**Ethernet1 Xplained Pro**. Ele monitora o gateway e o servidor pela rede,
mostra o estado do sistema e tenta recuperá-los quando falham.

Requisitos da proposta e como foram atendidos:

| Requisito | Implementação |
|---|---|
| Consultar periodicamente a saúde do gateway e do servidor via HTTP | A cada 2 s, `GET /health` nos dois e, com o servidor saudável, `GET /data` ([supervisor/src/main.c](supervisor/src/main.c)) |
| Mostrar no display o status, as últimas leituras e o log de eventos | Três telas no OLED 128×32: Status, Nós e Eventos ([supervisor/src/ui.c](supervisor/src/ui.c)) |
| Navegar com botões e sinalizar falhas com LEDs | Botão 1 volta, 2 avança, 3 rola. LED 1 = falha do gateway, LED 2 = falha do servidor, LED 3 pisca a cada ciclo |
| Após N timeouts consecutivos, marcar falha e tentar recuperar com `POST /reset` | Máquina de estados com N = 3 e carência de 10 s após o reset ([supervisor/src/monitor.c](supervisor/src/monitor.c)) |

## 2. Arquitetura do firmware

O firmware é escrito em C sobre o Zephyr e tem duas threads.

- **Thread de consulta** (`poll_thread`, [main.c](supervisor/src/main.c)):
  - acorda a cada `CONFIG_SUPERVISOR_POLL_MS` (2000 ms);
  - faz as requisições HTTP e alimenta a máquina de estados;
  - envia o `POST /reset` quando a máquina pede.
- **Thread `main`** ([ui.c](supervisor/src/ui.c)): desenha o display e trata
  botões e LEDs.

As duas threads compartilham uma estrutura de estado protegida por `k_mutex`
(`sup_lock`). A consulta acorda a interface pelo semáforo `redraw`. Sem aviso,
a interface redesenha a cada 1 s.

Os parâmetros ficam em [supervisor/Kconfig](supervisor/Kconfig):

| Símbolo | Padrão | Função |
|---|---|---|
| `SUPERVISOR_POLL_MS` | 2000 | período de consulta |
| `SUPERVISOR_TIMEOUT_MS` | 1500 | timeout de cada requisição |
| `SUPERVISOR_FAIL_THRESHOLD` | 3 | falhas consecutivas para marcar FALHA |
| `SUPERVISOR_RESET_GRACE_MS` | 10000 | janela após o reset em que falhas não contam |
| `SUPERVISOR_GATEWAY_ADDR/PORT`, `SUPERVISOR_SERVER_ADDR/PORT` | — | endereços dos serviços |

### 2.1 Cliente HTTP

[http.c](supervisor/src/http.c) é um cliente mínimo sobre os sockets do Zephyr.

- O tempo limite de cada requisição é aplicado com `SO_RCVTIMEO`.
- A leitura termina quando a resposta está completa, pelo `Content-Length` ou
  pelo fim do *chunked encoding*. Assim o cliente não espera o timeout à toa.
- O retorno é o código HTTP ou um erro negativo:
  - timeout;
  - `-EBADMSG`, para conexão fechada sem resposta.
- O JSON é interpretado com a biblioteca declarativa do Zephyr
  (`CONFIG_JSON_LIBRARY`).

### 2.2 Detecção de falhas

[monitor.c](supervisor/src/monitor.c) não depende do Zephyr, por isso pode ser
testado no PC.

- **Critério de vivo:** o subsistema está vivo quando responde HTTP 200. Uma
  resposta 200 zera o contador.
- **Contagem de falhas:** cada falha incrementa o contador, e o display mostra
  `?1`, `?2`.
- **Terceira falha consecutiva:**
  1. o subsistema é marcado em `FALHA`;
  2. o supervisor envia o `POST /reset`;
  3. abre-se a carência de 10 s, em que falhas não contam, porque a placa está
     reiniciando.
- **Reinício espontâneo:** quando o `uptime_ms` volta para trás, o supervisor
  registra "reiniciou".
- **Status `degraded`:** o servidor responde `"degraded"` quando perdeu o
  gateway. Isso aparece no display, mas não dispara reset, porque o problema
  está no gateway.

### 2.3 Interface no OLED1

As fontes padrão do Zephyr dariam só duas linhas de 12 caracteres no display de
128×32. O projeto traz uma fonte própria de 6×8 pixels
([font6x8.c](supervisor/src/font6x8.c)), que dá **4 linhas de 21
caracteres**. O display usa o *character framebuffer* (CFB) do Zephyr.

## 3. Driver Ethernet KSZ8851SNL

A Ethernet1 Xplained Pro usa o **Microchip KSZ8851SNL**, que **não tem driver
no Zephyr**. O grupo escreveu um driver fora da árvore:

- driver: [drivers/ethernet/ksz8851snl](drivers/ethernet/ksz8851snl/);
- binding de devicetree: [dts/bindings/ethernet/microchip,ksz8851snl.yaml](dts/bindings/ethernet/microchip,ksz8851snl.yaml).

Funcionamento do driver:

- **Acesso a registradores:** cada acesso começa com um comando SPI de 16 bits
  (opcode, *byte enable* e endereço).
- **Inicialização:**
  1. reset do chip;
  2. conferência do Chip ID (CIDER);
  3. configuração de transmissão, recepção, autonegociação e interrupções.
- **Recepção:** uma thread dedicada espera a interrupção de quadro recebido.
  Ela lê o quadro inteiro da fila (QMU) numa única transação SPI e o entrega à
  pilha TCP/IP.
- **Transmissão:** o pacote é escrito direto dos fragmentos do `net_pkt`, sem
  cópia intermediária.

Problemas de baixo nível resolvidos, todos comentados no código:

- **Interrupção por nível:** a interrupção do chip é por nível, mas o GPIO do
  SAMD21 só detecta borda. Por isso a thread também relê o status a cada
  100 ms.
- **Buffer nulo no SPI:** o `spi_sam0` dá *hard fault* com buffer de recepção
  `NULL` sem par de transmissão. Os bytes descartados vão para um buffer de
  rascunho (`scratch`).
- **Pilha da workqueue:** um vetor alocado na pilha estourava a *workqueue* do
  sistema. Ele foi para a memória do driver, e
  `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE` subiu para 1280.
- **Pino de reset:** o pino de reset do chip é o mesmo do botão SW0 da placa.
  Isso exigiu um reset em dreno aberto feito à mão.

## 4. Pinagem, rede e memória

### Pinagem

| Placa | Conector | Barramento SPI |
|---|---|---|
| OLED1 | **EXT1** | SERCOM próprio |
| Ethernet1 | **EXT3** | SERCOM próprio, diferente do OLED1 |

- **Por que o OLED1 não fica no EXT3:** no EXT3, dois sinais do OLED1 não
  chegam ao microcontrolador. Um LED está ligado ao chip select do Serial
  Flash, e um botão depende de jumper. A justificativa completa está em
  [supervisor/README.md](supervisor/README.md).
- **Serial Flash:** o chip select do Serial Flash divide o barramento com a
  Ethernet. Ele fica em nível alto por uma segunda entrada em `cs-gpios`.

### Rede

- O endereço vem por **DHCP**.
- `CONFIG_NET_CONFIG_INIT_TIMEOUT=1` evita que o display fique 30 s apagado no
  boot.

### Memória

O SAMD21 tem **32 KB de RAM**, e por isso
([supervisor/boards/samd21_xpro.conf](supervisor/boards/samd21_xpro.conf)):

- **Resolvedor de DNS desligado**, o que economiza cerca de 3 KB. Endereços
  literais continuam funcionando pelo `getaddrinfo`.
- **Pilhas ajustadas:** as pilhas das threads e as filas de pacotes foram
  dimensionadas uma a uma.
- **Log:** `CONFIG_LOG_MODE_MINIMAL` (cerca de 2 KB a menos).
- **`CONFIG_STACK_SENTINEL`:** como o SAMD21 não tem MPU, o sentinela
  transforma estouros de pilha silenciosos em pânico com mensagem.

## 5. Simulador do gateway e do servidor

Os quatro grupos trabalham em paralelo. Para não depender da entrega dos
outros, o grupo fez um simulador do contrato de interfaces da proposta:
[sim/](sim/), com o script [sim.py](sim/sim.py). Ele é escrito em Python só com
a biblioteca padrão e roda em Docker.

- **Gateway** na porta 5500: `GET /sensors`, `GET /health`, `POST /reset`.
- **Servidor** na porta 5501: `GET /health`, `GET /data`, `GET /log`,
  `POST /reset`.
- **Nós virtuais:** três nós sensores geram leituras a cada 500 ms. O servidor
  simulado grava CSV em `sd/log.csv`.
- **Reset:** `POST /reset` derruba o subsistema por 3 s e o traz de volta com o
  `uptime_ms` zerado.
- **Injeção de falhas** (`/_sim/fault`, fora do contrato):
  - `hang`: a requisição fica pendurada até o timeout;
  - `error`: resposta HTTP 500;
  - `close`: conexão derrubada.

  Também é possível tirar um nó do ar com `/_sim/node`.

Uso: [sim/README.md](sim/README.md).

## 6. Organização do repositório

O `CMakeLists.txt` da raiz era o de uma aplicação, o que impedia que `drivers/`
e `lib/` entrassem em qualquer build. A raiz passou a ser um **módulo Zephyr**:

- [zephyr/module.yml](zephyr/module.yml) declara `kconfig`, `dts_root` e
  `board_root`;
- cada aplicação fica na sua pasta (`app`, `blinky`, `gateway`, `supervisor`).

O `blinky` foi só movido de pasta nessa reorganização; ele não é do Grupo D.

Arquivos do Grupo D:

```text
supervisor/            firmware do supervisor
  src/                 main.c, http.c, monitor.c, ui.c, font6x8.c
  tests/test_monitor.c teste da máquina de estados (gcc no PC)
  boards/              configuração e overlay da SAM D21 Xplained Pro
  eth_ksz8851snl.overlay
sim/                   simulador do gateway e do servidor
drivers/ethernet/ksz8851snl/        driver Ethernet
dts/bindings/ethernet/microchip,ksz8851snl.yaml
zephyr/module.yml, Kconfig          estrutura de módulo
```

## 7. Testes e validação

| Teste | Como | Resultado |
|---|---|---|
| Máquina de estados | `gcc -I../src test_monitor.c ../src/monitor.c -o t && ./t` em `supervisor/tests` | Passa. O teste cobre a primeira resposta, as falhas abaixo de N, o reset na terceira falha, a carência, a volta com reinício e o reboot espontâneo |
| Simulador | `docker compose up` e `curl` nas rotas e nas falhas | Rotas, reset (3 s fora, `reset_count` 1), `hang`, `error` e CSV conforme descrito |
| Firmware no PC | `west build -b native_sim/native/64 supervisor` contra o simulador | Descrito em [supervisor/README.md](supervisor/README.md) |
| Placa contra o simulador | SAM D21 + OLED1 + Ethernet1, simulador no PC, mesma rede | Validado em 06/10/2026: driver, link, DHCP, TCP, HTTP, JSON, telas e ciclo de reset, sem descarte de quadros (`rx_err`, `rx_reject` e `spi_err` zerados) |
| Integração com o gateway real | Supervisor na placa consultando o gateway do grupo B | Teste feito junto com o grupo do gateway. Tudo funcionou como esperado |

Como compilar, gravar e depurar: [supervisor/README.md](supervisor/README.md).

## 8. Limitações conhecidas

- **Gerador de números aleatórios:** `CONFIG_TEST_RANDOM_GENERATOR` é usado
  porque o SAMD21 não tem TRNG. Ele serve para portas e ISN do TCP, mas não é
  seguro para criptografia. O projeto não usa TLS.
- **Reenvio do reset:** o reset é reenviado a cada N falhas mais a carência,
  sem *backoff* exponencial (comentário em `monitor.c`).
- **Conectores:** trocar o OLED1 e a Ethernet1 de conector exige reescrever os
  overlays.

## 9. Commits

| Commit | Data | Conteúdo |
|---|---|---|
| `efa1b2d` | 22/09/2026 | simulador do gateway e do servidor |
| `29998f2` | 22/09/2026 | firmware do supervisor |
| `d70165e` | 15/10/2026 | driver KSZ8851SNL, rede, pinagem e reorganização em módulo |

O código do Grupo D é trabalho conjunto dos três integrantes. Os commits foram
enviados pela conta `arthurportella`.

## 10. Licença

- **MIT:** o código escrito pelo Grupo D está sob a licença MIT, conforme
  [LICENSE](LICENSE). Isso vale para [supervisor/](supervisor/), [sim/](sim/) e
  [drivers/ethernet/](drivers/ethernet/), cujos arquivos têm cabeçalho
  `SPDX-License-Identifier: MIT`, além do binding
  [microchip,ksz8851snl.yaml](dts/bindings/ethernet/microchip,ksz8851snl.yaml).
- **Apache-2.0:** os arquivos herdados do
  [zephyrproject-rtos/example-application](https://github.com/zephyrproject-rtos/example-application)
  continuam sob Apache-2.0 ([LICENSE-APACHE-2.0](LICENSE-APACHE-2.0)). O mesmo
  vale para os demais arquivos com cabeçalho `SPDX-License-Identifier:
  Apache-2.0`.
- **Outros grupos:** o código dos outros grupos segue a licença que eles
  definirem.
