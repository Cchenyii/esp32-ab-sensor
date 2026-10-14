#include "network_link.h"
#include "ota_service.h"
#include "protocol.h"
#include "wifi_credentials.h"
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <cstring>

static const char *TAG = "network_link";
static constexpr uint16_t PORT = 8080;
static constexpr int64_t ACK_BUDGET_MS = 200;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static network_link_snapshot_t snapshot{};
struct WifiControl {
    bool started = false, associated = false, ready = false;
    uint32_t ip = 0, epoch = 0, disconnects = 0;
    uint16_t reason = 0;
};
static WifiControl wifi;
static bool initialized;
static esp_event_handler_instance_t wifi_handler, ip_handler;

static int64_t now_ms() { return esp_timer_get_time() / 1000; }
extern "C" void network_link_get_snapshot(network_link_snapshot_t *output) {
    if (!output) return;
    portENTER_CRITICAL(&state_lock); *output = snapshot; portEXIT_CRITICAL(&state_lock);
}
static void publish(const network_link_snapshot_t &value) {
    portENTER_CRITICAL(&state_lock); snapshot = value; portEXIT_CRITICAL(&state_lock);
}
static WifiControl wifi_control() {
    portENTER_CRITICAL(&state_lock); auto value = wifi; portEXIT_CRITICAL(&state_lock);
    return value;
}
static void on_event(void *, esp_event_base_t base, int32_t id, void *data) {
    // Event callbacks update state only. The network task owns every socket
    // and initiates retries; no blocking connect, recv or send in callbacks.
    portENTER_CRITICAL(&state_lock);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) wifi.started = true;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) wifi.associated = true;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi.associated = false; wifi.ready = false; wifi.ip = 0;
        wifi.reason = static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
        ++wifi.epoch; ++wifi.disconnects;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        if (!wifi.ready || wifi.ip != event->ip_info.ip.addr) ++wifi.epoch;
        wifi.ip = event->ip_info.ip.addr; wifi.ready = true;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        wifi.ready = false; wifi.ip = 0; ++wifi.epoch;
    }
    portEXIT_CRITICAL(&state_lock);
}
static void close_socket(int &fd) {
    if (fd < 0) return;
    shutdown(fd, SHUT_RDWR); close(fd); fd = -1;
}
static bool nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
static bool send_ack(int fd, uint32_t sequence, uint8_t status) {
    uint8_t frame[SensorProtocol::MAX_FRAME_SIZE];
    size_t length = SensorProtocol::encodeFrame(SensorProtocol::ACK, sequence,
                                                &status, 1, frame, sizeof(frame));
    size_t sent = 0;
    const int64_t deadline = now_ms() + ACK_BUDGET_MS;
    while (sent < length) {
        if (!wifi_control().ready || now_ms() >= deadline) return false;
        int n = send(fd, frame + sent, length - sent, 0); // fd is nonblocking
        if (n > 0) { sent += n; continue; }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return false;
        const int64_t remaining = deadline - now_ms();
        if (remaining <= 0) return false;
        fd_set write_set; FD_ZERO(&write_set); FD_SET(fd, &write_set);
        const int64_t wait = remaining < 50 ? remaining : 50;
        timeval timeout{0, static_cast<suseconds_t>(wait * 1000)};
        int result = select(fd + 1, nullptr, &write_set, nullptr, &timeout);
        if (result < 0 && errno != EINTR) return false;
    }
    return length != 0;
}
static int open_listener() {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!nonblocking(fd) || bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(fd, 2) != 0) { close_socket(fd); return -1; }
    return fd;
}
static bool configure_client(int fd) {
    if (!nonblocking(fd)) return false;
    int one = 1, idle = 5, interval = 1, count = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    return true;
}
static void network_task(void *) {
    network_link_snapshot_t state{};
    SensorProtocol::FrameParser parser;
    int listener = -1, client = -1;
    uint32_t seen_epoch = UINT32_MAX, seen_disconnects = 0, last_sequence = 0;
    bool has_sequence = false, connecting = false, mdns_running = false;
    unsigned retry = 0;
    int64_t next_attempt = 0, attempt_at = 0, last_new_frame = 0, last_rx = 0;
    int64_t next_listener = 0, last_stats = 0, next_mdns = 0;
    auto drop_client = [&](const char *reason) {
        if (client >= 0) {
            ESP_LOGI(TAG, "Client closed id=%" PRIu32 " reason=%s", state.connection_id, reason);
            ++state.disconnects;
        }
        close_socket(client); parser.reset(); has_sequence = false;
        state.client_connected = false; state.sensor_valid = false;
    };
    for (;;) {
        ota_service_touch(OTA_NETWORK);
        const int64_t now = now_ms();
        auto control = wifi_control();
        state.wifi_connected = control.ready;
        state.wifi_disconnects = control.disconnects;
        if (control.epoch != seen_epoch) {
            seen_epoch = control.epoch;
            drop_client("Wi-Fi/IP changed"); close_socket(listener);
            state.tcp_listening = false; state.ip[0] = 0;
            next_listener = now;
            if (control.ready) {
                inet_ntop(AF_INET, &control.ip, state.ip, sizeof(state.ip));
                ESP_LOGI(TAG, "Wi-Fi ready ip=%s; UART remains active", state.ip);
                retry = 0; connecting = false;
            }
        }
        if (control.disconnects != seen_disconnects) {
            seen_disconnects = control.disconnects;
            connecting = false;
            uint32_t delay = 1000u << (retry < 5 ? retry : 5);
            if (delay > 30000) delay = 30000;
            if (retry < 5) ++retry;
            next_attempt = now + delay;
            ESP_LOGW(TAG, "Wi-Fi disconnected reason=%u retry_in_ms=%" PRIu32,
                     control.reason, delay);
        }
        if (!control.ready) {
            if (control.started) {
                if (control.associated && !connecting) {
                    connecting = true; attempt_at = now;
                }
                if (connecting && now - attempt_at >= 30000) {
                    ESP_LOGW(TAG, "Wi-Fi connect/DHCP deadline; retrying");
                    esp_wifi_disconnect(); connecting = false; next_attempt = now + 1000;
                } else if (!control.associated && !connecting && now >= next_attempt) {
                    esp_err_t result = esp_wifi_connect();
                    ++state.wifi_attempts; attempt_at = now;
                    connecting = result == ESP_OK;
                    next_attempt = now + 1000;
                    ESP_LOGI(TAG, "Wi-Fi attempt=%" PRIu32 " result=%s",
                             state.wifi_attempts, esp_err_to_name(result));
                }
            }
            publish(state);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!mdns_running && now >= next_mdns) {
            esp_err_t err = mdns_init();
            if (err == ESP_OK) err = mdns_hostname_set("esp32-b");
            if (err == ESP_OK) err = mdns_instance_name_set("ESP32 B sensor receiver");
            if (err == ESP_OK) err = mdns_service_add(nullptr, "_esp32b", "_tcp", PORT, nullptr, 0);
            if (err == ESP_OK) {
                mdns_running = true;
                ESP_LOGI(TAG, "mDNS published esp32-b.local service=_esp32b._tcp port=%u", PORT);
            } else {
                mdns_free(); next_mdns = now + 5000;
                ESP_LOGW(TAG, "mDNS startup failed: %s", esp_err_to_name(err));
            }
        }
        if (listener < 0 && now >= next_listener) {
            listener = open_listener(); state.tcp_listening = listener >= 0;
            if (listener >= 0) ESP_LOGI(TAG, "TCP listening port=%u", PORT);
            else { next_listener = now + 1000; ESP_LOGW(TAG, "listen failed errno=%d", errno); }
        }
        if (client >= 0 && now - last_new_frame >= CONFIG_APP_TCP_VALID_TIMEOUT_MS) {
            ++state.timeouts;
            drop_client("no new valid sensor data");
        }
        if (state.sensor_valid && now - state.updated_ms >= CONFIG_APP_SENSOR_STALE_MS) {
            state.sensor_valid = false;
            ESP_LOGW(TAG, "Sensor stale; snapshot invalid");
        }
        if (parser.bufferedBytes() && now - last_rx > 1000) {
            parser.reset(); ++state.rejected;
        }

        fd_set read_set; FD_ZERO(&read_set); int highest = -1;
        if (listener >= 0) { FD_SET(listener, &read_set); highest = listener; }
        if (client >= 0) { FD_SET(client, &read_set); if (client > highest) highest = client; }
        timeval wait{0, 50000};
        int readable = highest < 0 ? 0 : select(highest + 1, &read_set, nullptr, nullptr, &wait);
        if (highest < 0) vTaskDelay(pdMS_TO_TICKS(50));
        if (readable < 0 && errno != EINTR) {
            drop_client("select error"); close_socket(listener); state.tcp_listening = false;
        }
        if (readable > 0 && listener >= 0 && FD_ISSET(listener, &read_set)) {
            sockaddr_in peer{}; socklen_t peer_size = sizeof(peer);
            int incoming = accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_size);
            if (incoming >= 0) {
                if (configure_client(incoming)) {
                    drop_client("replaced by new connection");
                    client = incoming; state.client_connected = true;
                    state.connection_id = ++state.connections;
                    last_new_frame = now_ms(); last_rx = last_new_frame;
                    char peer_ip[16]; inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
                    ESP_LOGI(TAG, "Client connected id=%" PRIu32 " peer=%s",
                             state.connection_id, peer_ip);
                } else close_socket(incoming);
            }
        }
        // At most 512 received bytes per turn keeps Wi-Fi events and deadlines
        // responsive even if the peer sends continuously.
        for (unsigned chunk = 0; client >= 0 && readable > 0 && chunk < 8; ++chunk) {
            uint8_t data[64];
            int n = recv(client, data, sizeof(data), 0);
            if (n == 0) { drop_client("peer closed"); break; }
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) drop_client("recv error");
                break;
            }
            last_rx = now_ms();
            if (parser.push(data, n) == SensorProtocol::ParseResult::BUFFER_OVERFLOW) ++state.rejected;
            for (;;) {
                SensorProtocol::Frame frame{};
                auto result = parser.next(frame);
                if (result == SensorProtocol::ParseResult::NONE) break;
                if (result != SensorProtocol::ParseResult::FRAME_READY) { ++state.rejected; continue; }
                if (frame.type != SensorProtocol::SENSOR_DATA) { ++state.rejected; continue; }
                SensorProtocol::SensorPayload sensor{};
                if (!SensorProtocol::decodeSensorPayload(frame, sensor) || sensor.humidityCentiPercent > 10000) {
                    ++state.rejected;
                    if (!send_ack(client, frame.sequence, SensorProtocol::ACK_LENGTH_ERROR)) {
                        ++state.ack_errors; drop_client("invalid-data ACK failed"); break;
                    }
                    ++state.acks; continue;
                }
                uint32_t delta = frame.sequence - last_sequence;
                bool duplicate = has_sequence && (delta == 0 || delta >= UINT32_C(0x80000000));
                if (duplicate) ++state.duplicates;
                else {
                    if (has_sequence && delta > 1) state.missed += delta - 1;
                    last_sequence = frame.sequence; has_sequence = true;
                    last_new_frame = now_ms();
                    state.sensor_valid = true; state.sequence = frame.sequence;
                    state.temperature_centi_c = sensor.temperatureCentiC;
                    state.humidity_centi_percent = sensor.humidityCentiPercent;
                    state.distance_cm = sensor.distanceCm; state.updated_ms = last_new_frame;
                    ++state.accepted;
                    ESP_LOGI(TAG, "SENSOR connection=%" PRIu32 " seq=%" PRIu32
                             " temp_centi=%d humidity_centi=%u distance_cm=%u",
                             state.connection_id, state.sequence, sensor.temperatureCentiC,
                             sensor.humidityCentiPercent, sensor.distanceCm);
                }
                if (!send_ack(client, frame.sequence, SensorProtocol::ACK_OK)) {
                    ++state.ack_errors; drop_client("ACK send deadline/error"); break;
                }
                ++state.acks;
            }
        }
        publish(state);
        if (now_ms() - last_stats >= 10000) {
            last_stats = now_ms();
            ESP_LOGI(TAG, "stats wifi=%d client=%d valid=%d accepted=%" PRIu32
                     " duplicates=%" PRIu32 " rejected=%" PRIu32 " missed=%" PRIu32
                     " acks=%" PRIu32 " ack_errors=%" PRIu32 " timeouts=%" PRIu32,
                     state.wifi_connected, state.client_connected, state.sensor_valid,
                     state.accepted, state.duplicates, state.rejected, state.missed,
                     state.acks, state.ack_errors, state.timeouts);
        }
    }
}
extern "C" esp_err_t network_link_start(void) {
    if (initialized) return ESP_ERR_INVALID_STATE;
    // The original secrets.h uses const char*; sizeof would measure the
    // pointer rather than the credential. Also accept example string macros.
    const size_t ssid_length = std::strlen(WIFI_SSID);
    const size_t password_length = std::strlen(WIFI_PASSWORD);
    if (!ssid_length || ssid_length > 32 || password_length > 64 ||
        (password_length && password_length < 8)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_flash_init(); // preserve NVS; do not erase on init failure
    if (err != ESP_OK) return err;
    err = esp_netif_init(); if (err != ESP_OK) return err;
    err = esp_event_loop_create_default(); if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (!netif) return ESP_ERR_NO_MEM;
    esp_netif_set_hostname(netif, "esp32-b");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init); if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr, &wifi_handler);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr, &ip_handler);
    if (err != ESP_OK) return err;
    wifi_config_t config{};
    std::memcpy(config.sta.ssid, WIFI_SSID, ssid_length);
    std::memcpy(config.sta.password, WIFI_PASSWORD, password_length);
    config.sta.threshold.authmode = password_length ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM); if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA); if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &config); if (err != ESP_OK) return err;
    err = esp_wifi_start(); if (err != ESP_OK) return err;
#ifdef CONFIG_APP_WIFI_MODEM_SLEEP
    const wifi_ps_type_t power_save = WIFI_PS_MIN_MODEM;
#else
    const wifi_ps_type_t power_save = WIFI_PS_NONE;
#endif
    err = esp_wifi_set_ps(power_save); if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "Wi-Fi modem sleep=%s", power_save == WIFI_PS_NONE ? "disabled" : "enabled");
    if (xTaskCreate(network_task, "wifi_tcp", 6144, nullptr, 3, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    initialized = true;
    ESP_LOGI(TAG, "Wi-Fi/TCP task started asynchronously; local tasks already running");
    return ESP_OK;
}
