#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool wifi_connected, tcp_listening, client_connected, sensor_valid;
    char ip[16];
    uint32_t connection_id, sequence;
    int16_t temperature_centi_c;
    uint16_t humidity_centi_percent, distance_cm;
    int64_t updated_ms;
    uint32_t wifi_attempts, wifi_disconnects, connections, disconnects;
    uint32_t accepted, duplicates, rejected, missed, acks, ack_errors, timeouts;
} network_link_snapshot_t;
esp_err_t network_link_start(void);
void network_link_get_snapshot(network_link_snapshot_t *output);
#ifdef __cplusplus
}
#endif
