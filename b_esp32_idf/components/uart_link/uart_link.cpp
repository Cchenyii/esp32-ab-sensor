#include "uart_link.h"
#include "ota_service.h"
#include "session_gate.h"
#include "network_link.h"
#include "sdkconfig.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

static constexpr uart_port_t PORT = UART_NUM_2;
static constexpr int RX_PIN = 16, TX_PIN = 17;
static const char *TAG = "uart_link";
static QueueHandle_t events;
static portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static uart_link_snapshot_t snapshot{};

extern "C" void uart_link_get_snapshot(uart_link_snapshot_t *output) {
    if (!output) return;
    portENTER_CRITICAL(&snapshot_lock);
    *output = snapshot;
    portEXIT_CRITICAL(&snapshot_lock);
}
static void publish(const uart_link_snapshot_t &state) {
    portENTER_CRITICAL(&snapshot_lock); snapshot = state; portEXIT_CRITICAL(&snapshot_lock);
}
static bool reply(const UartLink::Outcome &o) {
    uint8_t payload[9], frame[SW_MAX_FRAME];
    uint16_t length;
    if (o.replyType == SensorProtocol::ACK) { payload[0] = o.status; length = 1; }
    else {
        sw_write_token(payload, o.token);
        length = o.replyType == SW_OFFER ? 8 : 9;
        payload[8] = o.status;
    }
    size_t size = sw_encode(o.replyType, o.sequence, payload, length, frame, sizeof(frame));
    return size && uart_write_bytes(PORT, frame, size) == (int)size &&
           uart_wait_tx_done(PORT, pdMS_TO_TICKS(50)) == ESP_OK;
}
static uint8_t relay_flags(const network_link_snapshot_t &source, int64_t now) {
    uint8_t flags = 0;
    if (source.wifi_connected) flags |= SW_WIFI_READY;
    if (source.client_connected) flags |= SW_TCP_CONNECTED;
    if (source.accepted) flags |= SW_SENSOR_SEEN;
    if (source.sensor_valid && source.wifi_connected && source.client_connected &&
        now >= source.updated_ms && now - source.updated_ms < CONFIG_APP_SENSOR_STALE_MS)
        flags |= SW_SENSOR_VALID;
    return flags;
}
class SensorRelay {
public:
    void ack(const SensorProtocol::Frame &frame, uint64_t active,
             int64_t now, uart_link_snapshot_t &state) {
        if (frame.payloadLength != 9) { ++state.rejected; return; }
        if (!pending_ || session_ != active || frame.sequence != sequence_ ||
            sw_read_token(frame.payload) != session_) return;
        pending_ = false; next_ = now + 1000;
        if (frame.payload[8] == SW_OK) {
            ++state.relay_acks;
            ESP_LOGI(TAG, "RELAY ACK seq=%" PRIu32 " source_connection=%" PRIu32
                     " source_seq=%" PRIu32 " valid=%d", sequence_, source_.connection_id,
                     source_.sequence, !!(wire_flags_ & SW_SENSOR_VALID));
        } else {
            ++state.relay_failures;
            ESP_LOGW(TAG, "RELAY rejected seq=%" PRIu32 " status=%u", sequence_, frame.payload[8]);
        }
    }
    void poll(uint64_t active, int64_t now, uart_link_snapshot_t &state) {
        if (session_ != active) {
            session_ = active; pending_ = false; issued_ = false; next_ = 0; next_sequence_ = 1;
        }
        if (!active) return;
        network_link_snapshot_t latest{};
        network_link_get_snapshot(&latest);
        const uint8_t flags = relay_flags(latest, now);
        const bool changed = !issued_ || latest.connection_id != source_.connection_id ||
                             latest.sequence != source_.sequence || flags != last_flags_;
        // Only the latest sample is retained. New data/status supersedes an
        // unacknowledged old sample; a late ACK cannot confirm the replacement.
        if (changed || (!pending_ && now >= next_)) {
            source_ = latest; last_flags_ = flags; issued_ = true;
            sequence_ = next_sequence_++; if (!next_sequence_) ++next_sequence_;
            attempts_ = 0; pending_ = true;
            send(now, state);
        } else if (pending_ && now - last_tx_ >= 200) {
            if (attempts_ < 3) { ++state.relay_retries; send(now, state); }
            else {
                pending_ = false; ++state.relay_failures; next_ = now + 1000;
                ESP_LOGW(TAG, "RELAY ACK timeout seq=%" PRIu32, sequence_);
            }
        }
    }
private:
    void send(int64_t now, uart_link_snapshot_t &state) {
        sw_sensor_t data{};
        data.token = session_; data.source_connection = source_.connection_id;
        data.source_sequence = source_.sequence; data.flags = relay_flags(source_, now);
        if (data.flags & SW_SENSOR_VALID)
            data.valid_for_ms = CONFIG_APP_SENSOR_STALE_MS - (now - source_.updated_ms);
        data.temperature_centi_c = source_.temperature_centi_c;
        data.humidity_centi_percent = source_.humidity_centi_percent; data.distance_cm = source_.distance_cm;
        uint8_t frame[SW_MAX_FRAME];
        size_t size = sw_encode_sensor(sequence_, &data, frame, sizeof(frame));
        wire_flags_ = data.flags; last_tx_ = now; ++attempts_; ++state.relay_sent;
        if (!size || uart_write_bytes(PORT, frame, size) != (int)size ||
            uart_wait_tx_done(PORT, pdMS_TO_TICKS(50)) != ESP_OK) {
            ESP_LOGW(TAG, "RELAY TX error seq=%" PRIu32, sequence_);
        }
        if (attempts_ == 1)
            ESP_LOGI(TAG, "RELAY TX seq=%" PRIu32 " source_connection=%" PRIu32
                     " source_seq=%" PRIu32 " valid=%d temp_centi=%d humidity_centi=%u"
                     " distance_cm=%u", sequence_, data.source_connection, data.source_sequence,
                     !!(data.flags & SW_SENSOR_VALID), data.temperature_centi_c,
                     data.humidity_centi_percent, data.distance_cm);
    }
    network_link_snapshot_t source_{};
    uint64_t session_ = 0;
    uint32_t sequence_ = 0, next_sequence_ = 1;
    bool pending_ = false, issued_ = false;
    uint8_t last_flags_ = 0, wire_flags_ = 0;
    unsigned attempts_ = 0;
    int64_t last_tx_ = 0, next_ = 0;
};
static void link_task(void *) {
    uint64_t seed = ((uint64_t)esp_random() << 32) | esp_random();
    UartLink::SessionGate gate(seed, CONFIG_APP_UART_ALLOW_LEGACY);
    SensorProtocol::FrameParser parser;
    SensorRelay relay;
    uart_link_snapshot_t state{};
    int64_t last_rx = 0, last_stats = 0;
    for (;;) {
        ota_service_touch(OTA_UART);
        uart_event_t event{};
        if (xQueueReceive(events, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
                event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) {
                ++state.uart_errors;
                uart_flush_input(PORT); xQueueReset(events); parser.reset();
                ESP_LOGW(TAG, "RX error=%d; parser reset, session retained", (int)event.type);
            }
        }
        // Drain on events and timeouts: lost event notifications cannot strand bytes.
        size_t buffered = 0;
        ESP_ERROR_CHECK(uart_get_buffered_data_len(PORT, &buffered));
        while (buffered) {
            uint8_t chunk[64];
            int n = uart_read_bytes(PORT, chunk, buffered < sizeof(chunk) ? buffered : sizeof(chunk), 0);
            if (n <= 0) break;
            last_rx = esp_timer_get_time() / 1000;
            if (parser.push(chunk, n) == SensorProtocol::ParseResult::BUFFER_OVERFLOW) ++state.rejected;
            for (;;) {
                SensorProtocol::Frame f{};
                auto result = parser.next(f);
                if (result == SensorProtocol::ParseResult::NONE) break;
                if (result != SensorProtocol::ParseResult::FRAME_READY) { ++state.rejected; continue; }
                if (f.type == SW_SENSOR_ACK) {
                    relay.ack(f, gate.activeToken(), last_rx, state);
                    continue;
                }
                auto o = gate.handle(f, last_rx);
                if (o.newSession) {
                    state.valid = false; state.session_mode = true; state.session = o.token;
                    ESP_LOGI(TAG, "SESSION established token=%016" PRIx64, o.token);
                }
                if (o.accepted) {
                    state.valid = true; state.session_mode = o.token != 0;
                    state.session = o.token; state.sequence = o.sequence;
                    state.x = o.joystick.x; state.y = o.joystick.y;
                    state.direction = o.joystick.direction; state.sw = o.joystick.sw;
                    state.updated_ms = last_rx; ++state.accepted;
                    ESP_LOGI(TAG, "JOY mode=%s session=%016" PRIx64 " seq=%" PRIu32
                             " x=%u y=%u dir=%u sw=%u", o.token ? "session" : "legacy",
                             o.token, o.sequence, state.x, state.y, state.direction, state.sw);
                } else if (o.duplicate) ++state.duplicates;
                else if (f.type == SW_JOYSTICK || f.type == SensorProtocol::JOYSTICK_DATA) ++state.rejected;
                if (o.reply && !reply(o)) {
                    ++state.ack_errors; ESP_LOGW(TAG, "Reply TX failed");
                }
            }
            ESP_ERROR_CHECK(uart_get_buffered_data_len(PORT, &buffered));
        }
        const int64_t now = esp_timer_get_time() / 1000;
        if (parser.bufferedBytes() && now - last_rx > 1000) { parser.reset(); ++state.rejected; }
        if (state.valid && now - state.updated_ms > 3000) {
            state.valid = false; ESP_LOGW(TAG, "Joystick data timeout; snapshot invalid");
        }
        relay.poll(gate.activeToken(), now, state);
        publish(state);
        if (now - last_stats >= 10000) {
            last_stats = now;
            ESP_LOGI(TAG, "stats accepted=%" PRIu32 " duplicates=%" PRIu32
                     " rejected=%" PRIu32 " uart_errors=%" PRIu32 " ack_errors=%" PRIu32
                     " valid=%d relay_sent=%" PRIu32 " relay_acks=%" PRIu32
                     " relay_retries=%" PRIu32 " relay_failures=%" PRIu32,
                     state.accepted, state.duplicates, state.rejected,
                     state.uart_errors, state.ack_errors, state.valid, state.relay_sent,
                     state.relay_acks, state.relay_retries, state.relay_failures);
        }
    }
}
extern "C" esp_err_t uart_link_start(void) {
    if (events) return ESP_ERR_INVALID_STATE;
    if (!UartLink::session_self_test()) {
        ESP_LOGE(TAG, "Session self-test: FAIL"); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Session self-test: PASS (wire parser, handshake, reboot, dedup, wrap)");
    uart_config_t config{};
    config.baud_rate = 115200;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    esp_err_t err = uart_param_config(PORT, &config);
    if (err != ESP_OK) return err;
    err = uart_set_pin(PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(PORT, 2048, 0, 16, &events, 0);
    if (err != ESP_OK) return err;
    if (xTaskCreate(link_task, "stm32_uart", 4096, nullptr, 5, nullptr) != pdPASS) {
        uart_driver_delete(PORT); events = nullptr; return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UART2 started RX=%d TX=%d baud=115200 legacy=%d",
             RX_PIN, TX_PIN, CONFIG_APP_UART_ALLOW_LEGACY);
    return ESP_OK;
}
