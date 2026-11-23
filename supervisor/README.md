# Supervisor (grupo D)

SAM D21 Xplained Pro + OLED1 Xplained Pro (**EXT1**) + Ethernet1 Xplained Pro (**EXT3**).

A cada `CONFIG_SUPERVISOR_POLL_MS` (2 s): `GET /health` no gateway e no servidor,
`GET /data` no servidor. Depois de N timeouts/erros consecutivos (N = 3), marca
falha, acende o LED e manda `POST /reset`. Depois do reset espera
`CONFIG_SUPERVISOR_RESET_GRACE_MS` (10 s) antes de voltar a contar falhas.
Se o `uptime_ms` voltar para trás, registra "reiniciou".

"Vivo" = respondeu HTTP 200. O `status` do corpo (ex.: `"degraded"` do servidor
quando o gateway caiu) não dispara reset.

## Telas (OLED 128x32, 4 linhas)

| Botão | Ação |
|---|---|
| 1 | tela anterior |
| 2 | próxima tela |
| 3 | rola (próximo nó / eventos mais antigos) |

1. **Status**: estado de GW e SRV (`OK`, `?1` = falhou mas < N, `FALHA`), uptime, nós online, falhas e resets
2. **Nós**: luz, temperatura, aceleração, `last_seen_ms`, flags (de `GET /data`)
3. **Eventos**: `online`, `FALHA`, `reset ok/s/resp`, `reiniciou`, com o tempo em s

LED 1 = gateway em falha, LED 2 = servidor em falha, LED 3 = pisca a cada ciclo.

## Compilar e gravar

```bash
# só OLED1 (sem Ethernet): testa display, botões e LEDs; tudo fica em FALHA
west build -b samd21_xpro supervisor

# com a Ethernet1 Xplained Pro no EXT3
west build -b samd21_xpro supervisor -- -DEXTRA_DTC_OVERLAY_FILE=eth_ksz8851snl.overlay

west flash
```

O OLED1 vai no **EXT1** e a Ethernet1 no **EXT3**. Trocar os dois de conector
não funciona sem reescrever os overlays: cada um usa um SERCOM diferente.

### Por que o OLED1 fica no EXT1

No EXT3 dois sinais do OLED1 não chegam ao microcontrolador (notas 1 e 2 da
tabela 4-3, página 13 do [user guide da SAMD21 Xpro][ug]):

| Sinal | Pino do EXT3 | Problema |
|---|---|---|
| LED 2 | 8 (PA13) | de fábrica vai ao chip select do Serial Flash e fica desconectado do conector; só um resistor de 0 Ω movido de R314 para R313 resolve |
| BOTÃO 3 | 4 (PA03) | compartilhado com o USB ID; depende do jumper JS300 no J307 |

No EXT1 nenhum dos dez pinos usados tem função compartilhada, então os três
LEDs e os três botões funcionam sem mexer em nada na placa.

A Ethernet não se incomoda com o EXT3: ela usa os pinos 5, 6, 9 e 15-18, e os
únicos compartilhamentos ali são o LED0 (PB30) e o SW0 (PA15) da placa, tratados
em [eth_ksz8851snl.overlay](eth_ksz8851snl.overlay). O chip select do Serial
Flash (PA13), que divide o barramento SPI com ela, é mantido em nível alto por
uma segunda entrada em `cs-gpios`.

[ug]: https://ww1.microchip.com/downloads/en/DeviceDoc/Atmel-42220-SAMD21-Xplained-Pro_User-Guide.pdf

IPs e portas do gateway e do servidor em [prj.conf](prj.conf). Com o simulador,
os dois são o IP do PC, portas 5500 e 5501.

A placa pega o **próprio endereço por DHCP** (`CONFIG_NET_DHCPV4=y` em
[boards/samd21_xpro.conf](boards/samd21_xpro.conf)): é só plugar o cabo. As três
strings `CONFIG_NET_CONFIG_MY_IPV4_*` estão vazias de propósito — com string
vazia o `setup_ipv4()` do `net_config` não adiciona endereço nenhum e deixa
endereço, máscara e gateway virem do lease. Para voltar ao IP fixo, basta
preenchê-las.

Pinos do OLED1 (EXT1) em [boards/samd21_xpro.overlay](boards/samd21_xpro.overlay)
e os da Ethernet1 (EXT3) em [eth_ksz8851snl.overlay](eth_ksz8851snl.overlay).

## Testar sem placa

Lógica de falha (só gcc, sem Zephyr):

```bash
cd supervisor/tests
gcc -I../src test_monitor.c ../src/monitor.c -o t && ./t
```

O firmware inteiro no PC contra o simulador (native_sim usa os sockets do host):

```bash
apt-get install -y gcc make        # a imagem do devcontainer não traz gcc do host
cd sim && docker compose up -d && cd ..
west build -b native_sim/native/64 supervisor -d build-nsim
./build-nsim/zephyr/zephyr.exe
```

Em outro terminal, provocar falhas (ver [sim/README.md](../sim/README.md)):

```bash
curl -X POST "http://localhost:5500/_sim/fault?mode=hang&seconds=20"  # GW: FALHA, reset, volta online
curl -X POST http://localhost:5500/reset                               # GW: "reiniciou", sem FALHA
curl -X POST "http://localhost:5501/_sim/fault?mode=close&seconds=10" # SRV: FALHA, reset, volta
```

## Driver da Ethernet

O KSZ8851SNL da Ethernet1 Xplained Pro **não tem driver no Zephyr**. O driver
está neste repositório, em
[drivers/ethernet/ksz8851snl](../drivers/ethernet/ksz8851snl/), com o binding
em [dts/bindings/ethernet](../dts/bindings/ethernet/microchip,ksz8851snl.yaml).

Ele é habilitado sozinho pelo overlay (`CONFIG_ETH_KSZ8851SNL` tem
`default y` quando o nó existe no devicetree).

Se o log trouxer `KSZ8851SNL não encontrado: CIDER = 0x....`:

| Valor lido | Causa provável |
|---|---|
| `0x7088` | bytes invertidos: conferir `dipo`/`dopo` e o modo SPI |
| `0x0000` ou `0xffff` | fiação, chip select ou alimentação |
| outro | placa no conector errado |

## Estado em 06/10/2026

**Validado fim a fim na placa**, contra o simulador da pasta [sim/](../sim/)
rodando no PC, com placa e PC na mesma rede: driver KSZ8851SNL, link, DHCP,
TCP, HTTP, parsing do JSON, telas do OLED e o ciclo de reset. A recepção não
descarta quadro (`rx_err`, `rx_reject` e `spi_err` zerados).

Para repetir: `cd sim && docker compose up -d`, e trocar os endereços em
[prj.conf](prj.conf) pelo IP do PC.

**Integração com o gateway real:** testada junto com o grupo B (gateway),
funcionando como esperado. Para isso, ajustar `CONFIG_SUPERVISOR_GATEWAY_ADDR`
e `CONFIG_SUPERVISOR_GATEWAY_PORT` em [prj.conf](prj.conf) para o endereço do
gateway. O `45.182.14.16` versionado no `prj.conf` foi só um endereço de teste
de conexão.

## Depuração

Sem console, a inspeção é pelo depurador:

```powershell
pyocd gdbserver -t atsamd21j18a --persist --port 3333    # deixar rodando
arm-none-eabi-gdb -batch -nx -x script.gdb build-eth/zephyr/zephyr.elf
```

Passar os comandos **por arquivo**, não por `-ex` (o PowerShell quebra os
argumentos nas vírgulas), e não chamar funções do alvo pelo gdb: elas travam
esperando mutex de outra thread.

O driver mantém contadores em `eth_ksz8851_data_0.stats` (quadros vistos,
entregues, descartados por cada motivo, erros de SPI) — é por onde se descobre
onde um pacote se perde.
