#include <zephyr/ztest.h>
#include <errno.h>
#include "sensor_fetcher.h"

ZTEST(sensor_fetcher_suite, test_parse_valid_gateway_json)
{
    const char *json_payload = 
        "["
        "  {\"node_id\": 1, \"light\": 450, \"tempc\": 24.5, \"accel\": [0.1, 0.0, 9.8], \"last_seen_ms\": 1000},"
        "  {\"node_id\": 2, \"light\": 120, \"tempc\": 22.1, \"accel\": [0.0, -0.1, 9.7], \"last_seen_ms\": 2500}"
        "]";

    sensor_batch_t batch;
    int ret = sensor_fetcher_parse_json(json_payload, &batch);

    zassert_equal(ret, 0, "O parsing falhou com o código de erro: %d", ret);
    zassert_equal(batch.count, 2, "Esperado 2 sensores, encontrado: %zu", batch.count);

    /* Validação do Nó 1 */
    zassert_equal(batch.sensors[0].node_id, 1, "ID incorreto");
    zassert_equal(batch.sensors[0].light, 450, "Luminosidade incorreta");
    zassert_within(batch.sensors[0].tempc, 24.5f, 0.01f, "Temperatura incorreta");
    zassert_within(batch.sensors[0].accel[2], 9.8f, 0.01f, "Aceleração Z incorreta");
    zassert_equal(batch.sensors[0].last_seen_ms, 1000, "Timestamp incorreto");

    /* Validação do Nó 2 */
    zassert_equal(batch.sensors[1].node_id, 2, "ID incorreto");
    zassert_within(batch.sensors[1].tempc, 22.1f, 0.01f, "Temperatura incorreta");
}

ZTEST(sensor_fetcher_suite, test_parse_invalid_json)
{
    sensor_batch_t batch;
    int ret = sensor_fetcher_parse_json("{\"invalid\": json}", &batch);
    zassert_equal(ret, -EBADMSG, "Deveria retornar -EBADMSG para JSON malformatado");
}

ZTEST(sensor_fetcher_suite, test_parse_null_args)
{
    sensor_batch_t batch;
    zassert_equal(sensor_fetcher_parse_json(NULL, &batch), -EINVAL, "Deveria falhar com -EINVAL");
    zassert_equal(sensor_fetcher_parse_json("[]", NULL), -EINVAL, "Deveria falhar com -EINVAL");
}

ZTEST(sensor_fetcher_suite, test_parse_missing_fields)
{
    sensor_batch_t batch;
    const char *json = "[{\"node_id\": 1}]"; /* Faltam tempc, light, accel, last_seen_ms */
    int ret = sensor_fetcher_parse_json(json, &batch);
    zassert_equal(ret, 0, "Deveria ignorar itens incompletos sem quebrar o parser");
    zassert_equal(batch.count, 0, "Nenhum nó válido deveria ter sido contabilizado");
}

ZTEST_SUITE(sensor_fetcher_suite, NULL, NULL, NULL, NULL, NULL);
