#ifndef SENSOR_FETCHER_H_
#define SENSOR_FETCHER_H_

#include <stdint.h>
#include <stddef.h>

#define MAX_SENSORS 8

typedef struct {
    uint8_t node_id;
    uint16_t light;
    float tempc;
    float accel[3];
    uint32_t last_seen_ms;
} sensor_data_t;

typedef struct {
    sensor_data_t sensors[MAX_SENSORS];
    size_t count;
} sensor_batch_t;

/**
 * @brief Processa o payload JSON vindo do Gateway (GET /sensors).
 * 
 * @param json_str String com o JSON recebido.
 * @param out_batch Ponteiro para a estrutura onde as leituras serão guardadas.
 * @return int 0 em caso de sucesso, negativo em caso de erro.
 */
int sensor_fetcher_parse_json(const char *json_str, sensor_batch_t *out_batch);

#endif /* SENSOR_FETCHER_H_ */
