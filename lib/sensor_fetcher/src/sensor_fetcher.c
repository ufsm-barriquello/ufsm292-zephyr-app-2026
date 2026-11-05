#include "sensor_fetcher.h"
#include <cJSON.h>
#include <errno.h>
#include <string.h>

int sensor_fetcher_parse_json(const char *json_str, sensor_batch_t *out_batch)
{
    if (json_str == NULL || out_batch == NULL) {
        return -EINVAL;
    }

    memset(out_batch, 0, sizeof(sensor_batch_t));

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        return -EBADMSG;
    }

    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return -EBADMSG;
    }

    size_t count = 0;
    cJSON *item = NULL;

    cJSON_ArrayForEach(item, root) {
        if (count >= MAX_SENSORS) {
            break;
        }

        cJSON *node_id = cJSON_GetObjectItemCaseSensitive(item, "node_id");
        cJSON *light = cJSON_GetObjectItemCaseSensitive(item, "light");
        cJSON *tempc = cJSON_GetObjectItemCaseSensitive(item, "tempc");
        cJSON *accel = cJSON_GetObjectItemCaseSensitive(item, "accel");
        cJSON *last_seen = cJSON_GetObjectItemCaseSensitive(item, "last_seen_ms");

        if (!cJSON_IsNumber(node_id) || !cJSON_IsNumber(light) || 
            !cJSON_IsNumber(tempc) || !cJSON_IsArray(accel) || 
            !cJSON_IsNumber(last_seen)) {
            continue;
        }

        sensor_data_t *sensor = &out_batch->sensors[count];
        sensor->node_id = (uint8_t)node_id->valueint;
        sensor->light = (uint16_t)light->valueint;
        sensor->tempc = (float)tempc->valuedouble;
        sensor->last_seen_ms = (uint32_t)last_seen->valueint;

        /* Extrai os 3 eixos de aceleração [x, y, z] */
        for (int i = 0; i < 3; i++) {
            cJSON *accel_elem = cJSON_GetArrayItem(accel, i);
            if (cJSON_IsNumber(accel_elem)) {
                sensor->accel[i] = (float)accel_elem->valuedouble;
            } else {
                sensor->accel[i] = 0.0f;
            }
        }

        count++;
    }

    out_batch->count = count;
    cJSON_Delete(root);

    return 0;
}
