#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"
typedef struct {
    uint32_t magic, version, revision;
    int32_t temperature_high_centi;
    uint32_t humidity_high_centi, distance_low_cm, telemetry_period_ms;
} threshold_config_t;
/* Owned by cloud_task: no NVS work in MQTT callbacks. */
esp_err_t threshold_store_load(threshold_config_t *config, bool *persisted);
esp_err_t threshold_store_patch(threshold_config_t *config, const cJSON *patch, bool persisted,
                                bool *changed, const char **reason);
bool threshold_config_json(cJSON *object, const threshold_config_t *config);
