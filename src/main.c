#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "sensor_fetcher.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Exemplo de payload vindo periodicamente da interface de rede/HTTP */
static const char mock_gateway_response[] = 
    "["
    "  {\"node_id\": 1, \"light\": 300, \"tempc\": 25.39, \"accel\": [0.0, 0.0, 9.8], \"last_seen_ms\": 1000},"
    "  {\"node_id\": 2, \"light\": 120, \"tempc\": 22.15, \"accel\": [0.0, -0.1, 9.7], \"last_seen_ms\": 2500}"
    "]";

int main(void)
{
    LOG_INF("Iniciando Datalogger SAMD21 (Modo Contínuo)...");

    while (1) {
        sensor_batch_t batch;
        int ret = sensor_fetcher_parse_json(mock_gateway_response, &batch);

        if (ret == 0) {
            LOG_INF("--- Leitura Realizada (Nós: %zu) ---", batch.count);
            for (size_t i = 0; i < batch.count; i++) {
                float temp = batch.sensors[i].tempc;
                int temp_int = (int)temp;
                int temp_frac = (int)((temp - temp_int) * 100);
                if (temp_frac < 0) temp_frac = -temp_frac;

                LOG_INF("Node %d | Temp: %d.%02d C | Lux: %d | Last Seen: %u ms", 
                        batch.sensors[i].node_id, 
                        temp_int, 
                        temp_frac, 
                        batch.sensors[i].light,
                        batch.sensors[i].last_seen_ms);
            }
        } else {
            LOG_ERR("Falha ao processar payload: %d", ret);
        }

        /* Aguarda 5 segundos até a próxima requisição/leitura */
        k_msleep(5000);
    }

    return 0;
}
