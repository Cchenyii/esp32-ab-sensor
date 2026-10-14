#include "threshold_store.h"
#include "nvs.h"
#include <math.h>
#include <string.h>
#define CONFIG_MAGIC UINT32_C(0x54424346)
static bool valid(const threshold_config_t *c) {
    return c->magic == CONFIG_MAGIC && c->version == 1 && c->revision > 0 &&
           c->temperature_high_centi >= -4000 && c->temperature_high_centi <= 8000 &&
           c->humidity_high_centi <= 10000 && c->distance_low_cm >= 1 &&
           c->distance_low_cm <= 200 && c->telemetry_period_ms >= 1000 &&
           c->telemetry_period_ms <= 60000;
}
static esp_err_t save(const threshold_config_t *c) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("tb_cfg", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "thresholds", c, sizeof(*c));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
esp_err_t threshold_store_load(threshold_config_t *c, bool *persisted) {
    *c = (threshold_config_t){ CONFIG_MAGIC, 1, 1, 3500, 8000, 20, 2000 };
    *persisted = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("tb_cfg", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    threshold_config_t stored;
    size_t size = sizeof(stored);
    err = nvs_get_blob(handle, "thresholds", &stored, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = save(c); *persisted = err == ESP_OK; return err;
    }
    if (err != ESP_OK) return err;
    if (size != sizeof(stored) || !valid(&stored)) return ESP_ERR_INVALID_STATE;
    *c = stored; *persisted = true; return ESP_OK;
}
esp_err_t threshold_store_patch(threshold_config_t *c, const cJSON *patch, bool persisted,
                                bool *changed, const char **reason) {
    *changed = false; *reason = "invalid_object";
    if (!cJSON_IsObject(patch)) return ESP_ERR_INVALID_ARG;
    threshold_config_t candidate = *c;
    unsigned seen = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, patch) {
        unsigned bit = 0;
        double low = 0, high = 0, scale = 1;
        if (!entry->string) return ESP_ERR_INVALID_ARG;
        if (!strcmp(entry->string, "temperature_high_c")) { bit = 1; low = -40; high = 80; scale = 100; }
        else if (!strcmp(entry->string, "humidity_high_pct")) { bit = 2; high = 100; scale = 100; }
        else if (!strcmp(entry->string, "distance_low_cm")) { bit = 4; low = 1; high = 200; }
        else if (!strcmp(entry->string, "telemetry_period_ms")) { bit = 8; low = 1000; high = 60000; }
        else continue; /* Shared attributes can include unrelated keys. */
        if (seen & bit) { *reason = "duplicate_key"; return ESP_ERR_INVALID_ARG; }
        seen |= bit;
        if (!cJSON_IsNumber(entry) || !isfinite(entry->valuedouble) ||
            entry->valuedouble < low || entry->valuedouble > high) {
            *reason = "type_or_range"; return ESP_ERR_INVALID_ARG;
        }
        const double scaled = entry->valuedouble * scale;
        if (fabs(scaled - round(scaled)) > 0.00001) {
            *reason = "precision"; return ESP_ERR_INVALID_ARG;
        }
        const int32_t value = (int32_t)llround(scaled);
        if (bit == 1) candidate.temperature_high_centi = value;
        else if (bit == 2) candidate.humidity_high_centi = (uint32_t)value;
        else if (bit == 4) candidate.distance_low_cm = (uint32_t)value;
        else candidate.telemetry_period_ms = (uint32_t)value;
    }
    if (!seen) { *reason = "no_supported_keys"; return ESP_ERR_NOT_FOUND; }
    if (persisted && !memcmp(c, &candidate, sizeof(candidate))) { *reason = "unchanged"; return ESP_OK; }
    if (candidate.revision == UINT32_MAX) { *reason = "revision_exhausted"; return ESP_ERR_INVALID_STATE; }
    ++candidate.revision;
    if (!valid(&candidate)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = save(&candidate);
    if (err != ESP_OK) { *reason = "nvs_write_failed"; return err; }
    *c = candidate; /* Apply only after the single-blob NVS commit succeeds. */
    *changed = true; *reason = "saved"; return ESP_OK;
}
bool threshold_config_json(cJSON *o, const threshold_config_t *c) {
    return cJSON_AddNumberToObject(o, "config_revision", c->revision) &&
        cJSON_AddNumberToObject(o, "temperature_high_c", c->temperature_high_centi / 100.0) &&
        cJSON_AddNumberToObject(o, "humidity_high_pct", c->humidity_high_centi / 100.0) &&
        cJSON_AddNumberToObject(o, "distance_low_cm", c->distance_low_cm) &&
        cJSON_AddNumberToObject(o, "telemetry_period_ms", c->telemetry_period_ms);
}
