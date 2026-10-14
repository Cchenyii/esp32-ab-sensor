#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool valid;
    bool session_mode;
    uint64_t session;
    uint32_t sequence;
    uint16_t x, y;
    uint8_t direction, sw;
    int64_t updated_ms;
    uint32_t accepted, duplicates, rejected, uart_errors, ack_errors;
    uint32_t relay_sent, relay_acks, relay_retries, relay_failures;
} uart_link_snapshot_t;
esp_err_t uart_link_start(void);
void uart_link_get_snapshot(uart_link_snapshot_t *output);
#ifdef __cplusplus
}
#endif
