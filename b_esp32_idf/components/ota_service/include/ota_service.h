#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { OTA_HEARTBEAT, OTA_GPIO, OTA_UART, OTA_NETWORK, OTA_CLOUD, OTA_TASK_COUNT } ota_task_id_t;
struct cJSON;
void ota_service_touch(ota_task_id_t task);
void ota_service_protocol_ready(void);
esp_err_t ota_service_start(void);
/* Key is returned only through the trusted USB console, never HTTP/MQTT. */
esp_err_t ota_service_arm(char key[65]);
bool ota_service_status_json(struct cJSON *json);
#ifdef __cplusplus
}
#endif
