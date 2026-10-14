#include "cloud_link.h"
#include "threshold_store.h"
#include "cloud_settings.h"
#include "network_link.h"
#include "uart_link.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "ota_service.h"

#define BODY_LIMIT 1024
#define TOPIC_LIMIT 128
static const char *TAG = "cloud_link";
static const char *ATTR = "v1/devices/me/attributes";
static const char *ATTR_RESPONSE = "v1/devices/me/attributes/response/";
static const char *RPC_REQUEST = "v1/devices/me/rpc/request/";
typedef struct {
    int kind, id, length;
    uint32_t epoch;
    char topic[TOPIC_LIMIT], body[BODY_LIMIT + 1];
} cloud_message_t;
typedef struct {
    bool connected, failed;
    uint32_t epoch, errors, drops;
} cloud_control_t;
enum { MESSAGE_DATA = 1, MESSAGE_SUBACK, MESSAGE_PUBACK };
static portMUX_TYPE control_lock = portMUX_INITIALIZER_UNLOCKED;
static cloud_control_t control;
static QueueHandle_t messages;
static bool initialized;
static cloud_control_t get_control(void) {
    portENTER_CRITICAL(&control_lock);
    cloud_control_t copy = control;
    portEXIT_CRITICAL(&control_lock); return copy;
}
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void time_synced(struct timeval *value) {
    ESP_LOGI(TAG, "Clock synchronized utc_unix=%" PRId64, (int64_t)value->tv_sec);
}
static void dropped(void) {
    portENTER_CRITICAL(&control_lock); ++control.drops; portEXIT_CRITICAL(&control_lock);
}
static void on_mqtt(void *argument, esp_event_base_t base, int32_t id, void *data) {
    (void)argument; (void)base;
    esp_mqtt_event_handle_t event = data;
    /* Fragment assembly belongs to the sole esp-mqtt callback task. */
    static cloud_message_t assembling;
    static int received;
    if (id == MQTT_EVENT_CONNECTED) {
        portENTER_CRITICAL(&control_lock); control.connected = true; portEXIT_CRITICAL(&control_lock);
        received = 0; assembling.length = 0;
    } else if (id == MQTT_EVENT_DISCONNECTED || id == MQTT_EVENT_ERROR) {
        portENTER_CRITICAL(&control_lock);
        control.connected = false; control.failed = true;
        if (id == MQTT_EVENT_ERROR) ++control.errors;
        portEXIT_CRITICAL(&control_lock);
        if (id == MQTT_EVENT_ERROR && event->error_handle) {
            const esp_mqtt_error_codes_t *error = event->error_handle;
            ESP_LOGW(TAG, "MQTT error type=%d tls_esp=0x%x tls_stack=%d cert_flags=%d socket_errno=%d broker_code=%d",
                     error->error_type, (unsigned)error->esp_tls_last_esp_err,
                     error->esp_tls_stack_err, error->esp_tls_cert_verify_flags,
                     error->esp_transport_sock_errno, error->connect_return_code);
        }
        received = 0; assembling.length = 0;
    } else if (id == MQTT_EVENT_SUBSCRIBED || id == MQTT_EVENT_PUBLISHED) {
        if (id == MQTT_EVENT_SUBSCRIBED && event->data && event->data_len > 0) {
            for (int i = 0; i < event->data_len; ++i) if ((unsigned char)event->data[i] == 0x80) {
                portENTER_CRITICAL(&control_lock); control.failed = true; portEXIT_CRITICAL(&control_lock);
                return;
            }
        }
        /* Use the same bounded queue so a fast ACK cannot race ID assignment. */
        static cloud_message_t ack;
        ack.kind = id == MQTT_EVENT_SUBSCRIBED ? MESSAGE_SUBACK : MESSAGE_PUBACK;
        ack.id = event->msg_id; ack.epoch = get_control().epoch;
        if (xQueueSend(messages, &ack, 0) != pdTRUE) dropped();
    } else if (id == MQTT_EVENT_DATA) {
        if (event->current_data_offset == 0) {
            received = 0; assembling.length = 0;
            if (event->total_data_len <= 0 || event->total_data_len > BODY_LIMIT ||
                event->topic_len <= 0 || event->topic_len >= TOPIC_LIMIT || !event->topic) {
                dropped(); return;
            }
            assembling.kind = MESSAGE_DATA; assembling.epoch = get_control().epoch;
            assembling.length = event->total_data_len;
            memcpy(assembling.topic, event->topic, event->topic_len);
            assembling.topic[event->topic_len] = 0;
        }
        if (!assembling.length || event->total_data_len != assembling.length ||
            event->current_data_offset != received || event->data_len < 0 ||
            event->data_len > assembling.length - received ||
            (event->data_len && !event->data)) {
            assembling.length = 0; received = 0; dropped(); return;
        }
        if (event->data_len) memcpy(assembling.body + received, event->data, event->data_len);
        received += event->data_len;
        if (received == assembling.length) {
            assembling.body[received] = 0;
            if (memchr(assembling.body, 0, received) ||
                xQueueSend(messages, &assembling, 0) != pdTRUE) dropped();
            assembling.length = 0; received = 0;
        }
    }
}
/* Bound nesting before cJSON parsing to protect the worker stack. */
static cJSON *parse_json(const char *body) {
    int depth = 0;
    bool string = false, escape = false;
    for (const unsigned char *p = (const unsigned char *)body; *p; ++p) {
        if (string) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '"') string = false;
        } else if (*p == '"') string = true;
        else if (*p == '{' || *p == '[') { if (++depth > 8) return NULL; }
        else if (*p == '}' || *p == ']') { if (--depth < 0) return NULL; }
    }
    if (depth || string) return NULL;
    const char *end = NULL;
    return cJSON_ParseWithLengthOpts(body, strlen(body) + 1, &end, true);
}
static void log_config(const threshold_config_t *config, bool persisted) {
    ESP_LOGI(TAG, "CONFIG revision=%" PRIu32 " temperature_high_centi=%" PRId32
             " humidity_high_centi=%" PRIu32 " distance_low_cm=%" PRIu32
             " telemetry_period_ms=%" PRIu32 " persisted=%d",
             config->revision, config->temperature_high_centi, config->humidity_high_centi,
             config->distance_low_cm, config->telemetry_period_ms, persisted);
}
static int queue_json(esp_mqtt_client_handle_t client, const char *topic, cJSON *json) {
    if (!client || !get_control().connected || !json) return -1;
    char *body = cJSON_PrintUnformatted(json);
    if (!body) return -1;
    /* No socket writes from local TCP/UART or event callbacks. Bounded outbox. */
    const int result = esp_mqtt_client_enqueue(client, topic, body, 0, 1, 0, true);
    cJSON_free(body);
    return result;
}
static bool config_attributes(esp_mqtt_client_handle_t client, const threshold_config_t *config) {
    cJSON *json = cJSON_CreateObject();
    bool ok = json && threshold_config_json(json, config) &&
              queue_json(client, ATTR, json) >= 0;
    cJSON_Delete(json); return ok;
}
static bool request_shared(esp_mqtt_client_handle_t client, uint32_t request) {
    char topic[96];
    snprintf(topic, sizeof(topic), "v1/devices/me/attributes/request/%" PRIu32, request);
    const char *body = "{\"sharedKeys\":\"temperature_high_c,humidity_high_pct,distance_low_cm,telemetry_period_ms\"}";
    return esp_mqtt_client_enqueue(client, topic, body, 0, 1, 0, true) >= 0;
}
static int telemetry(esp_mqtt_client_handle_t client, const threshold_config_t *config,
                     const network_link_snapshot_t *n, const uart_link_snapshot_t *u) {
    const int64_t time = now_ms();
    const bool sensor_valid = n->sensor_valid && n->wifi_connected && n->client_connected &&
                              time >= n->updated_ms && time - n->updated_ms <= 6000;
    const bool joy_valid = u->valid && time >= u->updated_ms && time - u->updated_ms <= 3000;
    cJSON *json = cJSON_CreateObject();
    if (!json) return -1;
    bool ok = cJSON_AddNumberToObject(json, "uptime_ms", (double)time) &&
        cJSON_AddBoolToObject(json, "sensor_valid", sensor_valid) &&
        cJSON_AddBoolToObject(json, "joystick_valid", joy_valid) &&
        cJSON_AddBoolToObject(json, "a_connected", n->client_connected) &&
        cJSON_AddNumberToObject(json, "config_revision", config->revision);
    if (sensor_valid) {
        ok = ok && cJSON_AddNumberToObject(json, "temperature", n->temperature_centi_c / 100.0) &&
            cJSON_AddNumberToObject(json, "humidity", n->humidity_centi_percent / 100.0) &&
            cJSON_AddNumberToObject(json, "distance_cm", n->distance_cm) &&
            cJSON_AddNumberToObject(json, "a_sequence", n->sequence) &&
            cJSON_AddNumberToObject(json, "a_connection", n->connection_id) &&
            cJSON_AddNumberToObject(json, "sensor_age_ms", (double)(time - n->updated_ms)) &&
            cJSON_AddBoolToObject(json, "alarm_temp_high", n->temperature_centi_c > config->temperature_high_centi) &&
            cJSON_AddBoolToObject(json, "alarm_humidity_high", n->humidity_centi_percent > config->humidity_high_centi) &&
            cJSON_AddBoolToObject(json, "alarm_distance_low", n->distance_cm < config->distance_low_cm);
    }
    if (joy_valid) {
        ok = ok && cJSON_AddNumberToObject(json, "joy_x", u->x) &&
            cJSON_AddNumberToObject(json, "joy_y", u->y) &&
            cJSON_AddNumberToObject(json, "joy_direction", u->direction) &&
            cJSON_AddNumberToObject(json, "joy_sw", u->sw);
    }
    int result = ok ? queue_json(client, "v1/devices/me/telemetry", json) : -1;
    cJSON_Delete(json); return result;
}
static bool numeric_suffix(const char *topic, const char *prefix) {
    const size_t length = strlen(prefix);
    if (strncmp(topic, prefix, length) || !topic[length]) return false;
    for (const unsigned char *p = (const unsigned char *)topic + length; *p; ++p)
        if (!isdigit(*p)) return false;
    return true;
}
static void handle_data(const cloud_message_t *message, esp_mqtt_client_handle_t client,
                        threshold_config_t *config, bool *persisted, bool *attributes_dirty) {
    cJSON *json = parse_json(message->body);
    if (!cJSON_IsObject(json)) { cJSON_Delete(json); ESP_LOGW(TAG, "Remote JSON rejected"); return; }
    const cJSON *patch = NULL;
    bool rpc = numeric_suffix(message->topic, RPC_REQUEST);
    bool get = false;
    if (rpc) {
        const cJSON *method = cJSON_GetObjectItemCaseSensitive(json, "method");
        if (cJSON_IsString(method) && !strcmp(method->valuestring, "getOTAStatus")) {
            char response_topic[TOPIC_LIMIT];
            snprintf(response_topic, sizeof(response_topic), "v1/devices/me/rpc/response/%s",
                     message->topic + strlen(RPC_REQUEST));
            cJSON *response = cJSON_CreateObject();
            if (!response || !cJSON_AddBoolToObject(response, "success", true) ||
                !ota_service_status_json(response) || queue_json(client, response_topic, response) < 0)
                ESP_LOGW(TAG, "OTA status reply could not be queued");
            cJSON_Delete(response); cJSON_Delete(json); return;
        }
        if (cJSON_IsString(method) && !strcmp(method->valuestring, "getConfig")) get = true;
        else if (cJSON_IsString(method) && !strcmp(method->valuestring, "setConfig"))
            patch = cJSON_GetObjectItemCaseSensitive(json, "params");
    } else if (!strcmp(message->topic, ATTR)) patch = json;
    else if (numeric_suffix(message->topic, ATTR_RESPONSE))
        patch = cJSON_GetObjectItemCaseSensitive(json, "shared");
    const char *reason = "unsupported_method_or_payload";
    bool changed = false;
    esp_err_t err = get ? ESP_OK : patch
        ? threshold_store_patch(config, patch, *persisted, &changed, &reason) : ESP_ERR_INVALID_ARG;
    if (changed) {
        *persisted = true; *attributes_dirty = true;
        ESP_LOGI(TAG, "Remote config saved"); log_config(config, *persisted);
    } else if (patch && err != ESP_OK && err != ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "Remote config rejected reason=%s", reason);
    if (rpc) {
        char response_topic[TOPIC_LIMIT];
        snprintf(response_topic, sizeof(response_topic), "v1/devices/me/rpc/response/%s",
                 message->topic + strlen(RPC_REQUEST));
        cJSON *response = cJSON_CreateObject();
        bool ok = response && cJSON_AddBoolToObject(response, "success", err == ESP_OK) &&
            cJSON_AddStringToObject(response, "status", get ? "current" : reason) &&
            cJSON_AddBoolToObject(response, "persisted", *persisted) &&
            threshold_config_json(response, config);
        if (!ok || queue_json(client, response_topic, response) < 0)
            ESP_LOGW(TAG, "RPC reply could not be queued");
        cJSON_Delete(response);
    }
    cJSON_Delete(json);
}
static void console_poll(threshold_config_t *config, bool *persisted, bool *dirty) {
    static char line[BODY_LIMIT + 1];
    static size_t used;
    static bool overflow;
    unsigned char bytes[128];
    for (int round = 0; round < 8; ++round) {
        int count = uart_read_bytes(UART_NUM_0, bytes, sizeof(bytes), 0);
        if (count <= 0) break;
        for (int i = 0; i < count; ++i) {
            if (bytes[i] == '\n' || bytes[i] == '\r') {
                if (overflow) ESP_LOGW(TAG, "Console command too long; ignored");
                else if (used) {
                    line[used] = 0;
                    if (!strcmp(line, "OTAARM")) {
                        char key[65];
                        esp_err_t result = ota_service_arm(key);
                        if (result == ESP_OK) printf("OTA_ARM {\"key\":\"%s\",\"ttl_s\":300}\n", key);
                        else printf("OTA_ARM {\"error\":\"%s\"}\n", esp_err_to_name(result));
                        memset(key, 0, sizeof(key));
                    }
                    else if (!strcmp(line, "OTAGET")) {
                        cJSON *status = cJSON_CreateObject();
                        char *body = ota_service_status_json(status) ? cJSON_PrintUnformatted(status) : NULL;
                        if (body) { printf("OTA_STATUS %s\n", body); cJSON_free(body); }
                        cJSON_Delete(status);
                    }
                    else if (!strcmp(line, "TBGET")) log_config(config, *persisted);
                    else if (!strncmp(line, "TBSET ", 6)) {
                        cJSON *json = parse_json(line + 6);
                        bool changed = false; const char *reason;
                        esp_err_t err = threshold_store_patch(config, json, *persisted, &changed, &reason);
                        cJSON_Delete(json);
                        if (changed) { *persisted = true; *dirty = true; }
                        ESP_LOGI(TAG, "TBSET result=%s reason=%s changed=%d",
                                 esp_err_to_name(err), reason, changed);
                        log_config(config, *persisted);
                    } else ESP_LOGW(TAG, "Console commands: TBGET / TBSET {JSON} / OTAGET / OTAARM");
                }
                used = 0; overflow = false;
            } else if (!overflow) {
                if (!bytes[i] || used == BODY_LIMIT) overflow = true;
                else line[used++] = (char)bytes[i];
            }
        }
    }
}
static esp_mqtt_client_handle_t create_client(bool tls, const char *uri, const char *token) {
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = token,
        .session.keepalive = 30,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .network.timeout_ms = 3000,
        .network.disable_auto_reconnect = true,
        .task.priority = 2, .task.stack_size = 6144,
        .buffer.size = 512, .buffer.out_size = 1024,
        .outbox.limit = 4096,
    };
    if (tls) cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (!client) return NULL;
    if (esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_mqtt, NULL) != ESP_OK ||
        esp_mqtt_client_start(client) != ESP_OK) {
        esp_mqtt_client_destroy(client); return NULL;
    }
    return client;
}
static void cloud_task(void *argument) {
    (void)argument;
    threshold_config_t config;
    bool persisted = false, attributes_dirty = true;
    esp_err_t nvs_result = nvs_flash_init(); /* No automatic erase on errors. */
    if (nvs_result == ESP_OK) nvs_result = threshold_store_load(&config, &persisted);
    else { bool ignored; (void)threshold_store_load(&config, &ignored); }
    if (nvs_result != ESP_OK) ESP_LOGW(TAG, "NVS unavailable/invalid: %s; using defaults without erase",
                                     esp_err_to_name(nvs_result));
    log_config(&config, persisted);
    bool console = false;
    if (!uart_is_driver_installed(UART_NUM_0))
        console = uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0) == ESP_OK;
    else console = true;
    ESP_LOGI(TAG, "Local config console %s: TBGET / TBSET {JSON}", console ? "ready" : "unavailable");
    const char *uri = cloud_broker_uri(), *token = cloud_access_token();
    const bool tls = !strncmp(uri, "mqtts://", 8);
    const bool plain = !strncmp(uri, "mqtt://", 7);
    const bool configured = uri[0] && token[0] && strlen(uri) < 256 && strlen(token) <= 256 &&
        !strchr(uri, '@') && !strchr(uri, '?') && !strchr(uri, '#') &&
        (tls || (plain && cloud_allow_plaintext()));
    if (!configured) ESP_LOGW(TAG, "MQTT awaiting local ThingsBoard credentials; local links/config remain active");
    esp_mqtt_client_handle_t client = NULL;
    bool announced = false, sntp_started = false, shared_requested = false;
    int subscription_ids[3] = {-1, -1, -1}, pending_telemetry = -1;
    unsigned subscription_mask = 0, retry_ms = 1000;
    int64_t next_attempt = 0, next_sntp_attempt = 0, connect_started = 0, connected_since = 0, last_send = 0, pending_since = 0, last_stats = 0;
    uint32_t attempts = 0, connections = 0, telemetry_sent = 0, telemetry_acks = 0, telemetry_skipped = 0;
    cloud_message_t message;
    for (;;) {
        ota_service_touch(OTA_CLOUD);
        if (console) console_poll(&config, &persisted, &attributes_dirty);
        network_link_snapshot_t network; network_link_get_snapshot(&network);
        cloud_control_t state = get_control();
        int64_t tick = now_ms();
        if (client && (!network.wifi_connected || state.failed ||
                       (!state.connected && tick - connect_started >= 12000))) {
            /* A fresh client discards the old outbox; offline samples are not replayed. */
            esp_mqtt_client_stop(client); esp_mqtt_client_destroy(client); client = NULL;
            portENTER_CRITICAL(&control_lock);
            control.connected = false; control.failed = false; ++control.epoch;
            portEXIT_CRITICAL(&control_lock);
            announced = false; pending_telemetry = -1;
            const unsigned delay = retry_ms;
            next_attempt = now_ms() + delay;
            if (retry_ms < 30000) retry_ms = retry_ms >= 16000 ? 30000 : retry_ms * 2;
            ESP_LOGW(TAG, "MQTT offline; next retry delay=%u ms", delay);
        }
        if (configured && tls && network.wifi_connected && !sntp_started && tick >= next_sntp_attempt) {
            esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
                ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "time.cloudflare.com", "pool.ntp.org"));
            sntp.sync_cb = time_synced;
            esp_err_t result = esp_netif_sntp_init(&sntp);
            sntp_started = result == ESP_OK || result == ESP_ERR_INVALID_STATE;
            next_sntp_attempt = now_ms() + 30000;
            ESP_LOGI(TAG, "SNTP init=%s; primary=ntp.aliyun.com; TLS awaits valid clock",
                     esp_err_to_name(result));
        }
        if (configured && network.wifi_connected && !client && tick >= next_attempt &&
            (!tls || time(NULL) > 1700000000)) {
            portENTER_CRITICAL(&control_lock);
            control.connected = false; control.failed = false; ++control.epoch;
            portEXIT_CRITICAL(&control_lock);
            connect_started = now_ms(); ++attempts;
            client = create_client(tls, uri, token);
            if (!client) { next_attempt = now_ms() + retry_ms; if (retry_ms < 30000) retry_ms = retry_ms >= 16000 ? 30000 : retry_ms * 2; }
        }
        state = get_control();
        if (client && state.connected && !announced) {
            ++connections; announced = true; attributes_dirty = true;
            shared_requested = false; subscription_mask = 0; retry_ms = 1000; last_send = 0;
            connected_since = now_ms();
            subscription_ids[0] = esp_mqtt_client_subscribe(client, ATTR, 1);
            subscription_ids[1] = esp_mqtt_client_subscribe(client, "v1/devices/me/attributes/response/+", 1);
            subscription_ids[2] = esp_mqtt_client_subscribe(client, "v1/devices/me/rpc/request/+", 1);
            ESP_LOGI(TAG, "MQTT connected; subscribing to ThingsBoard attributes/RPC");
            for (unsigned i = 0; i < 3; ++i) if (subscription_ids[i] < 0) {
                portENTER_CRITICAL(&control_lock); control.failed = true; portEXIT_CRITICAL(&control_lock);
            }
        }
        if (xQueueReceive(messages, &message, pdMS_TO_TICKS(20)) == pdTRUE) {
            state = get_control();
            if (client && state.connected && message.epoch == state.epoch) {
                if (message.kind == MESSAGE_DATA) handle_data(&message, client, &config, &persisted, &attributes_dirty);
                else if (message.kind == MESSAGE_SUBACK) {
                    for (unsigned i = 0; i < 3; ++i)
                        if (message.id == subscription_ids[i]) subscription_mask |= 1U << i;
                } else if (message.kind == MESSAGE_PUBACK && message.id == pending_telemetry) {
                    pending_telemetry = -1; ++telemetry_acks;
                }
            }
        }
        state = get_control(); tick = now_ms();
        /* CONNECTED may arrive during xQueueReceive. Initialize this connection
         * on the next loop before checking deadlines or publishing anything. */
        if (client && state.connected && announced && !state.failed) {
            if (subscription_mask != 7 && tick - connected_since > 10000) {
                ESP_LOGW(TAG, "MQTT subscription deadline exceeded; mask=%u", subscription_mask);
                portENTER_CRITICAL(&control_lock); control.failed = true; portEXIT_CRITICAL(&control_lock);
            } else if (subscription_mask == 7) {
                if (!shared_requested) shared_requested = request_shared(client, connections);
                if (attributes_dirty) attributes_dirty = !config_attributes(client, &config);
                if (pending_telemetry >= 0 && tick - pending_since > 6000) {
                    /* Close on a missing PUBACK, allowing reconnect to send only a fresh snapshot. */
                    ESP_LOGW(TAG, "MQTT telemetry PUBACK deadline exceeded");
                    portENTER_CRITICAL(&control_lock); control.failed = true; portEXIT_CRITICAL(&control_lock);
                    ++telemetry_skipped;
                } else if (pending_telemetry < 0 && tick - last_send >= config.telemetry_period_ms) {
                    uart_link_snapshot_t joystick; uart_link_get_snapshot(&joystick);
                    network_link_get_snapshot(&network);
                    pending_telemetry = telemetry(client, &config, &network, &joystick);
                    last_send = tick; pending_since = tick;
                    if (pending_telemetry >= 0) ++telemetry_sent; else ++telemetry_skipped;
                }
            }
        }
        if (tick - last_stats >= 30000) {
            last_stats = tick;
            ESP_LOGI(TAG, "stats configured=%d connected=%d clock_ready=%d utc_unix=%" PRId64 " attempts=%" PRIu32
                     " connections=%" PRIu32 " telemetry=%" PRIu32 " pubacks=%" PRIu32
                     " skipped=%" PRIu32 " rx_drops=%" PRIu32 " errors=%" PRIu32,
                     configured, state.connected, !tls || time(NULL) > 1700000000, (int64_t)time(NULL), attempts, connections, telemetry_sent,
                     telemetry_acks, telemetry_skipped, state.drops, state.errors);
        }
    }
}
esp_err_t cloud_link_start(void) {
    if (initialized) return ESP_ERR_INVALID_STATE;
    messages = xQueueCreate(6, sizeof(cloud_message_t));
    if (!messages) return ESP_ERR_NO_MEM;
    if (xTaskCreate(cloud_task, "tb_cloud", 8192, NULL, 2, NULL) != pdPASS) {
        vQueueDelete(messages); messages = NULL; return ESP_ERR_NO_MEM;
    }
    initialized = true; return ESP_OK;
}
