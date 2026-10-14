#include <inttypes.h>
#include <stdlib.h>

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol_self_test.h"
#include "uart_link.h"
#include "network_link.h"
#include "cloud_link.h"
#include "ota_service.h"
#include "esp_app_desc.h"

static const char *TAG = "bringup";

static void verify_protocol(void)
{
    sensor_protocol_self_test_report_t report = {0};
    const bool all_passed = sensor_protocol_run_self_tests(&report);
    unsigned passed = 0;
    for (size_t index = 0; index < report.count; ++index) {
        if (report.results[index].passed) ++passed;
        ESP_LOGI("protocol", "test %s: %s", report.results[index].name,
                 report.results[index].passed ? "PASS" : "FAIL");
    }
    ESP_LOGI("protocol", "Protocol self-test: %s (%u/%u)",
             all_passed ? "PASS" : "FAIL", passed, (unsigned)report.count);
    if (!all_passed) {
        ESP_LOGE("protocol", "Protocol validation failed; stopping startup");
        abort();
    }
    ota_service_protocol_ready();
}

static void heartbeat_task(void *argument)
{
    (void)argument;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t count = 0;

    for (;;) {
        ota_service_touch(OTA_HEARTBEAT);
        ESP_LOGI(TAG, "heartbeat=%" PRIu32 " free_heap_bytes=%" PRIu32
                      " minimum_heap_bytes=%" PRIu32,
                 ++count, esp_get_free_heap_size(),
                 esp_get_minimum_free_heap_size());
        vTaskDelayUntil(&last_wake,
                        pdMS_TO_TICKS(CONFIG_APP_HEARTBEAT_PERIOD_MS));
    }
}

static void button_task(void *argument)
{
    (void)argument;
    const gpio_num_t pin = (gpio_num_t)CONFIG_APP_BOOT_BUTTON_GPIO;
    int stable_level = gpio_get_level(pin);
    int candidate_level = stable_level;
    unsigned consecutive_samples = 0;
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "button gpio=%d initial=%s", (int)pin,
             stable_level ? "RELEASED" : "PRESSED");

    for (;;) {
        ota_service_touch(OTA_GPIO);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));
        const int level = gpio_get_level(pin);
        if (level != candidate_level) {
            candidate_level = level;
            consecutive_samples = 1;
        } else if (consecutive_samples < 3) {
            ++consecutive_samples;
        }

        if (consecutive_samples >= 3 && stable_level != candidate_level) {
            stable_level = candidate_level;
            ESP_LOGI(TAG, "button gpio=%d %s", (int)pin,
                     stable_level ? "RELEASED" : "PRESSED");
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 B bring-up; app=%s target=%s idf=%s",
             esp_app_get_description()->version, CONFIG_IDF_TARGET, esp_get_idf_version());
    ESP_LOGI(TAG, "reset_reason=%d console_baud=115200",
             (int)esp_reset_reason());
    verify_protocol();
    ESP_ERROR_CHECK(uart_link_start());

    const int pin = CONFIG_APP_BOOT_BUTTON_GPIO;
    if (!GPIO_IS_VALID_GPIO(pin) || (pin >= 6 && pin <= 11)) {
        ESP_LOGE(TAG, "Invalid button GPIO %d; check menuconfig", pin);
        abort();
    }

    const gpio_config_t input_config = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_config));

    if (xTaskCreate(heartbeat_task, "heartbeat", 3072, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create heartbeat task");
        abort();
    }
    if (xTaskCreate(button_task, "button", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create button task");
        abort();
    }
    ESP_LOGI(TAG, "FreeRTOS heartbeat and GPIO tasks started");
    const esp_err_t network_result = network_link_start();
    if (network_result != ESP_OK) {
        ESP_LOGE(TAG, "Network startup failed: %s; UART/GPIO remain running",
                 esp_err_to_name(network_result));
    }
    const esp_err_t cloud_result = cloud_link_start();
    if (cloud_result != ESP_OK) {
        ESP_LOGE(TAG, "Cloud startup failed: %s; local tasks remain running",
                 esp_err_to_name(cloud_result));
    }
    ESP_ERROR_CHECK(ota_service_start());
}
