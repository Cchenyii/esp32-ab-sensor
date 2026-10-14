#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_PROTOCOL_SELF_TEST_COUNT 8

typedef struct {
    const char *name;
    bool passed;
} sensor_protocol_test_result_t;

typedef struct {
    size_t count;
    sensor_protocol_test_result_t results[SENSOR_PROTOCOL_SELF_TEST_COUNT];
} sensor_protocol_self_test_report_t;

/* Pure in-memory checks. No Arduino, ESP-IDF, sockets or UART dependency. */
bool sensor_protocol_run_self_tests(sensor_protocol_self_test_report_t *report);

#ifdef __cplusplus
}
#endif
