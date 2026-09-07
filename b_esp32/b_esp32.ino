// Board B: TCP server and OLED display

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <U8g2lib.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_task_wdt.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include "protocol.h"
#include "secrets.h"
#include "display_msg.h"

static void enableTcpKeepAlive(WiFiClient &c) {
  int yes = 1;
  c.setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
  int idle = 5, intvl = 1, cnt = 3;
  c.setSocketOption(IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  c.setSocketOption(IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  c.setSocketOption(IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

static bool writeCompleteFrame(WiFiClient& connection,
                               const uint8_t* frame, size_t frameLength) {
  size_t offset = 0;
  const uint32_t deadline = millis() + 500;
  while (offset < frameLength && connection.connected() &&
         static_cast<int32_t>(deadline - millis()) > 0) {
    const size_t written =
        connection.write(frame + offset, frameLength - offset);
    if (written > 0) {
      offset += written;
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  return offset == frameLength;
}

static bool sendAcknowledgement(WiFiClient& connection, uint32_t sequence,
                                SensorProtocol::AckStatus status) {
  const uint8_t payload[] = {static_cast<uint8_t>(status)};
  uint8_t frame[SensorProtocol::MAX_FRAME_SIZE];
  const size_t length = SensorProtocol::encodeFrame(
      SensorProtocol::ACK, sequence, payload, sizeof(payload),
      frame, sizeof(frame));
  return length > 0 && writeCompleteFrame(connection, frame, length);
}

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

#define FW_VERSION "1.2.0"

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, 22, 21, U8X8_PIN_NONE);
WiFiServer server(8080);
WiFiClient client;
WebServer otaServer(80);

enum class OtaUiState : uint8_t {
  IDLE = 0,
  RECEIVING,
  SUCCESS,
  FAILED,
};

static volatile OtaUiState otaUiState = OtaUiState::IDLE;
static volatile uint8_t otaProgressPercent = 0;
static uint32_t otaFailedAt = 0;

static void drawOtaScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  const OtaUiState state = otaUiState;
  if (state == OtaUiState::RECEIVING) {
    u8g2.drawStr(0, 14, "OTA Update");
    u8g2.drawFrame(0, 24, 128, 12);
    const uint8_t fill =
        static_cast<uint8_t>((otaProgressPercent * 124) / 100);
    if (fill > 0) {
      u8g2.drawBox(2, 26, fill, 8);
    }
    char pct[12];
    snprintf(pct, sizeof(pct), "%u%%", otaProgressPercent);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 52, pct);
  } else if (state == OtaUiState::SUCCESS) {
    u8g2.drawStr(0, 20, "OTA OK");
    u8g2.drawStr(0, 36, "Rebooting...");
  } else if (state == OtaUiState::FAILED) {
    u8g2.drawStr(0, 20, "OTA Failed");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 36, Update.errorString());
  }
  u8g2.sendBuffer();
}

static void handleOtaRoot() {
  String page = F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<title>ESP32-B OTA</title></head><body>"
      "<h1>ESP32-B Firmware OTA</h1>"
      "<p>Version: ");
  page += FW_VERSION;
  page += F(
      "</p><form method='POST' action='/update' "
      "enctype='multipart/form-data'>"
      "<input type='file' name='firmware' accept='.bin'>"
      "<br><br><input type='submit' value='Upload &amp; Flash'>"
      "</form></body></html>");
  otaServer.send(200, "text/html", page);
}

static void handleOtaUpload() {
  HTTPUpload& upload = otaServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaUiState = OtaUiState::RECEIVING;
    otaProgressPercent = 0;
    Serial.printf("OTA start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaUiState = OtaUiState::FAILED;
      otaFailedAt = millis();
      Serial.printf("OTA begin failed: %s\n", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (otaUiState != OtaUiState::RECEIVING) return;
    if (Update.write(upload.buf, upload.currentSize) !=
        upload.currentSize) {
      otaUiState = OtaUiState::FAILED;
      otaFailedAt = millis();
      Serial.printf("OTA write failed: %s\n", Update.errorString());
      return;
    }
    const size_t total = Update.size();
    if (total > 0) {
      otaProgressPercent = static_cast<uint8_t>(
          min(100UL, (Update.progress() * 100UL) / total));
    } else if (otaProgressPercent < 95) {
      otaProgressPercent++;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaUiState != OtaUiState::RECEIVING) return;
    if (Update.end(true)) {
      otaProgressPercent = 100;
      otaUiState = OtaUiState::SUCCESS;
      Serial.printf("OTA complete: %u bytes\n", upload.totalSize);
    } else {
      otaUiState = OtaUiState::FAILED;
      otaFailedAt = millis();
      Serial.printf("OTA end failed: %s\n", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaUiState = OtaUiState::FAILED;
    otaFailedAt = millis();
    Serial.println("OTA aborted");
  }
}

static void handleOtaFinished() {
  if (otaUiState == OtaUiState::SUCCESS) {
    otaServer.send(200, "text/plain", "OK");
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP.restart();
  } else {
    otaServer.send(500, "text/plain", Update.errorString());
  }
}

static void confirmRunningFirmware() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("OTA image marked valid");
  }
}

void OtaTask(void* pv) {
  otaServer.on("/", HTTP_GET, handleOtaRoot);
  otaServer.on("/update", HTTP_POST, handleOtaFinished, handleOtaUpload);
  otaServer.begin();
  Serial.println("HTTP OTA: http://<ip>/");

  while (1) {
    if (WiFi.status() == WL_CONNECTED) {
      otaServer.handleClient();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

typedef struct {
  uint32_t validFrames;
  uint32_t crcErrors;
  uint32_t lengthErrors;
  uint32_t versionErrors;
  uint32_t bufferOverflows;
  uint32_t duplicateFrames;
  uint32_t missedFrames;
  uint32_t unknownTypes;
  uint32_t acknowledgementsSent;
  uint32_t acknowledgementFailures;
  uint32_t joystickFrames;
} ProtocolStats;

QueueHandle_t displayQueue;
ProtocolStats protocolStats = {};

static const char* joyDirName(uint8_t direction) {
  switch (direction) {
    case SensorProtocol::JOY_LEFT:
      return "LEFT";
    case SensorProtocol::JOY_RIGHT:
      return "RIGHT";
    case SensorProtocol::JOY_UP:
      return "UP";
    case SensorProtocol::JOY_DOWN:
      return "DOWN";
    default:
      return "CENTER";
  }
}

static bool writeCompleteUartFrame(HardwareSerial& port, const uint8_t* frame,
                                   size_t frameLength) {
  size_t offset = 0;
  const uint32_t deadline = millis() + 200;
  while (offset < frameLength &&
         static_cast<int32_t>(deadline - millis()) > 0) {
    const size_t written = port.write(frame + offset, frameLength - offset);
    if (written > 0) {
      offset += written;
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  return offset == frameLength;
}

static bool sendUartAcknowledgement(HardwareSerial& port, uint32_t sequence,
                                    SensorProtocol::AckStatus status) {
  const uint8_t payload[] = {static_cast<uint8_t>(status)};
  uint8_t frame[SensorProtocol::MAX_FRAME_SIZE];
  const size_t length = SensorProtocol::encodeFrame(
      SensorProtocol::ACK, sequence, payload, sizeof(payload), frame,
      sizeof(frame));
  return length > 0 && writeCompleteUartFrame(port, frame, length);
}

static void enqueueDisplay(const DisplayMsg& msg) {
  if (xQueueSend(displayQueue, &msg, 0) != pdTRUE) {
    DisplayMsg discarded;
    xQueueReceive(displayQueue, &discarded, 0);
    xQueueSend(displayQueue, &msg, 0);
  }
}

void ServerTask(void* pv) {
  DisplayMsg data = {};
  TickType_t lastRx = xTaskGetTickCount();
  SensorProtocol::FrameParser parser;
  SensorProtocol::Frame frame;
  bool hasLastSequence = false;
  uint32_t lastSequence = 0;
  uint32_t lastStatsPrint = millis();
  bool watchdogRegistered = esp_task_wdt_status(NULL) == ESP_OK;
  if (!watchdogRegistered) {
    watchdogRegistered = esp_task_wdt_add(NULL) == ESP_OK;
  }
  //#region agent log
  if (!watchdogRegistered) {
    Serial.println("ServerTask watchdog registration failed");
  }
  //#endregion

  while (1) {
    if (watchdogRegistered) esp_task_wdt_reset();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      int retries = 0;
      while (WiFi.status() != WL_CONNECTED && retries < 20) {
        if (watchdogRegistered) esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
        retries++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        MDNS.end();
        MDNS.begin("esp32-b");
        MDNS.addService("esp32b", "tcp", 8080);
      }
      client.stop();
      parser.reset();
      hasLastSequence = false;
      continue;
    }

    // Always accept pending clients (replace zombie sockets)
    {
      WiFiClient incoming = server.available();
      if (incoming) {
        client.stop();
        client = incoming;
        client.setTimeout(50);
        client.setNoDelay(true);
        enableTcpKeepAlive(client);
        parser.reset();
        hasLastSequence = false;
        Serial.println("Client connected");
        lastRx = xTaskGetTickCount();
      }
    }

    if (client.connected() && (xTaskGetTickCount() - lastRx) > pdMS_TO_TICKS(20000)) {
      client.stop();
      parser.reset();
      hasLastSequence = false;
      vTaskDelay(pdMS_TO_TICKS(200));
      Serial.println("Timeout, disconnect");
      continue;
    }

    // TCP is a byte stream: feed arbitrary chunks to the ring-buffer parser.
    uint8_t receiveBuffer[64];
    while (client.connected() && client.available()) {
      const size_t requested =
          min(static_cast<size_t>(client.available()), sizeof(receiveBuffer));
      const int received = client.read(receiveBuffer, requested);
      if (received <= 0) break;

      if (parser.push(receiveBuffer, received) ==
          SensorProtocol::ParseResult::BUFFER_OVERFLOW) {
        protocolStats.bufferOverflows++;
      }

      while (true) {
        const SensorProtocol::ParseResult result = parser.next(frame);
        if (result == SensorProtocol::ParseResult::NONE) break;

        if (result == SensorProtocol::ParseResult::CRC_ERROR) {
          protocolStats.crcErrors++;
          continue;
        }
        if (result == SensorProtocol::ParseResult::LENGTH_ERROR) {
          protocolStats.lengthErrors++;
          continue;
        }
        if (result == SensorProtocol::ParseResult::VERSION_ERROR) {
          protocolStats.versionErrors++;
          continue;
        }
        if (result == SensorProtocol::ParseResult::BUFFER_OVERFLOW) {
          continue;
        }
        if (frame.type != SensorProtocol::SENSOR_DATA) {
          protocolStats.unknownTypes++;
          continue;
        }

        SensorProtocol::SensorPayload sensor;
        if (!SensorProtocol::decodeSensorPayload(frame, sensor)) {
          protocolStats.lengthErrors++;
          continue;
        }

        lastRx = xTaskGetTickCount();
        if (hasLastSequence) {
          const uint32_t delta = frame.sequence - lastSequence;
          if (delta == 0 || delta >= 0x80000000UL) {
            protocolStats.duplicateFrames++;
            if (sendAcknowledgement(client, frame.sequence,
                                    SensorProtocol::ACK_OK)) {
              protocolStats.acknowledgementsSent++;
            } else {
              protocolStats.acknowledgementFailures++;
            }
            continue;
          }
          if (delta > 1) protocolStats.missedFrames += delta - 1;
        }
        hasLastSequence = true;
        lastSequence = frame.sequence;
        protocolStats.validFrames++;

        data = {};
        data.kind = 0;
        data.temp = sensor.temperatureCentiC / 100.0f;
        data.humi = sensor.humidityCentiPercent / 100.0f;
        data.dist = sensor.distanceCm;
        enqueueDisplay(data);
        if (sendAcknowledgement(client, frame.sequence,
                                SensorProtocol::ACK_OK)) {
          protocolStats.acknowledgementsSent++;
        } else {
          protocolStats.acknowledgementFailures++;
        }
      }
    }

    if (millis() - lastStatsPrint >= 60000) {
      lastStatsPrint = millis();
      Serial.printf(
          "Protocol frames=%lu joy=%lu crc=%lu len=%lu ver=%lu overflow=%lu "
          "duplicate=%lu missed=%lu unknown=%lu ack=%lu ackFail=%lu "
          "client=%d wifi=%d\n",
          (unsigned long)protocolStats.validFrames,
          (unsigned long)protocolStats.joystickFrames,
          (unsigned long)protocolStats.crcErrors,
          (unsigned long)protocolStats.lengthErrors,
          (unsigned long)protocolStats.versionErrors,
          (unsigned long)protocolStats.bufferOverflows,
          (unsigned long)protocolStats.duplicateFrames,
          (unsigned long)protocolStats.missedFrames,
          (unsigned long)protocolStats.unknownTypes,
          (unsigned long)protocolStats.acknowledgementsSent,
          (unsigned long)protocolStats.acknowledgementFailures,
          client.connected() ? 1 : 0,
          WiFi.status() == WL_CONNECTED ? 1 : 0);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void UartLinkTask(void* pv) {
  SensorProtocol::FrameParser parser;
  SensorProtocol::Frame frame;
  bool hasLastSequence = false;
  uint32_t lastSequence = 0;
  bool watchdogRegistered = esp_task_wdt_status(NULL) == ESP_OK;
  if (!watchdogRegistered) {
    watchdogRegistered = esp_task_wdt_add(NULL) == ESP_OK;
  }

  while (1) {
    if (watchdogRegistered) esp_task_wdt_reset();

    uint8_t chunk[64];
    size_t n = 0;
    while (Serial2.available() && n < sizeof(chunk)) {
      chunk[n++] = static_cast<uint8_t>(Serial2.read());
    }
    if (n > 0) {
      if (parser.push(chunk, n) ==
          SensorProtocol::ParseResult::BUFFER_OVERFLOW) {
        protocolStats.bufferOverflows++;
      }
    }

    while (true) {
      const SensorProtocol::ParseResult result = parser.next(frame);
      if (result == SensorProtocol::ParseResult::NONE) break;
      if (result == SensorProtocol::ParseResult::CRC_ERROR) {
        protocolStats.crcErrors++;
        sendUartAcknowledgement(Serial2, 0, SensorProtocol::ACK_CRC_ERROR);
        continue;
      }
      if (result == SensorProtocol::ParseResult::LENGTH_ERROR) {
        protocolStats.lengthErrors++;
        continue;
      }
      if (result == SensorProtocol::ParseResult::VERSION_ERROR) {
        protocolStats.versionErrors++;
        continue;
      }
      if (result == SensorProtocol::ParseResult::BUFFER_OVERFLOW) continue;

      if (frame.type != SensorProtocol::JOYSTICK_DATA) {
        protocolStats.unknownTypes++;
        sendUartAcknowledgement(Serial2, frame.sequence,
                                SensorProtocol::ACK_TYPE_ERROR);
        continue;
      }

      SensorProtocol::JoystickPayload joy;
      if (!SensorProtocol::decodeJoystickPayload(frame, joy)) {
        protocolStats.lengthErrors++;
        continue;
      }

      if (hasLastSequence) {
        const uint32_t delta = frame.sequence - lastSequence;
        if (delta == 0 || delta >= 0x80000000UL) {
          protocolStats.duplicateFrames++;
          sendUartAcknowledgement(Serial2, frame.sequence,
                                  SensorProtocol::ACK_OK);
          protocolStats.acknowledgementsSent++;
          continue;
        }
        if (delta > 1) protocolStats.missedFrames += delta - 1;
      }
      hasLastSequence = true;
      lastSequence = frame.sequence;
      protocolStats.validFrames++;
      protocolStats.joystickFrames++;

      DisplayMsg msg = {};
      msg.kind = 1;
      msg.joyX = joy.x;
      msg.joyY = joy.y;
      msg.joyDir = joy.direction;
      msg.joySw = joy.sw;
      enqueueDisplay(msg);

      if (sendUartAcknowledgement(Serial2, frame.sequence,
                                  SensorProtocol::ACK_OK)) {
        protocolStats.acknowledgementsSent++;
      } else {
        protocolStats.acknowledgementFailures++;
      }

      Serial.printf("JOY seq=%lu X=%u Y=%u %s SW=%u\n",
                    (unsigned long)frame.sequence, joy.x, joy.y,
                    joyDirName(joy.direction), joy.sw);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void DisplayTask(void* pv) {
  DisplayMsg data;
  char buf[32];
  TickType_t lastData = xTaskGetTickCount();
  bool watchdogRegistered = esp_task_wdt_status(NULL) == ESP_OK;
  if (!watchdogRegistered) {
    watchdogRegistered = esp_task_wdt_add(NULL) == ESP_OK;
  }
  //#region agent log
  if (!watchdogRegistered) {
    Serial.println("DisplayTask watchdog registration failed");
  }
  //#endregion

  while (1) {
    if (watchdogRegistered) esp_task_wdt_reset();
    if (otaUiState != OtaUiState::IDLE) {
      if (otaUiState == OtaUiState::FAILED && otaFailedAt > 0 &&
          millis() - otaFailedAt > 10000) {
        otaUiState = OtaUiState::IDLE;
        otaFailedAt = 0;
      } else {
        drawOtaScreen();
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
    }
    // Poll every 500ms; switch to Waiting after three missed 2s samples.
    if (xQueueReceive(displayQueue, &data, pdMS_TO_TICKS(500)) == pdTRUE) {
      lastData = xTaskGetTickCount();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_ncenB08_tr);
      if (data.kind == 1) {
        u8g2.drawStr(0, 16, "Joystick UART");
        u8g2.setFont(u8g2_font_6x10_tf);
        snprintf(buf, sizeof(buf), "X:%u Y:%u", data.joyX, data.joyY);
        u8g2.drawStr(0, 32, buf);
        snprintf(buf, sizeof(buf), "Dir:%s", joyDirName(data.joyDir));
        u8g2.drawStr(0, 44, buf);
        snprintf(buf, sizeof(buf), "SW:%u", data.joySw);
        u8g2.drawStr(0, 56, buf);
      } else {
        u8g2.drawStr(0, 16, "Sensor Data");
        u8g2.setFont(u8g2_font_6x10_tf);
        snprintf(buf, sizeof(buf), "Temp: %.1f C", data.temp);
        u8g2.drawStr(0, 32, buf);
        snprintf(buf, sizeof(buf), "Humi: %.1f %%", data.humi);
        u8g2.drawStr(0, 44, buf);
        snprintf(buf, sizeof(buf), "Dist: %d cm", data.dist);
        u8g2.drawStr(0, 56, buf);
      }
      u8g2.sendBuffer();
    } else {
      // Sensor period is 2s. Waiting at exactly 2s caused false alarms from
      // ordinary scheduling/network jitter (observed idle_ms=2087).
      // Show Waiting only after three consecutive expected frames are missed.
      if ((xTaskGetTickCount() - lastData) > pdMS_TO_TICKS(6000)) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB08_tr);
        if (WiFi.status() != WL_CONNECTED) {
          u8g2.drawStr(0, 16, "No WiFi");
        } else if (!client.connected()) {
          u8g2.drawStr(0, 16, "No client");
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.drawStr(0, 32, WiFi.localIP().toString().c_str());
          u8g2.drawStr(0, 48, "UART joy ready");
        } else {
          u8g2.drawStr(0, 16, "Waiting...");
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.drawStr(0, 32, "TCP ok, no data");
        }
        u8g2.sendBuffer();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  // STM32 V1 UART link: GPIO16=RX2, GPIO17=TX2 @ 115200
  Serial2.begin(115200, SERIAL_8N1, 16, 17);
  confirmRunningFirmware();
  Serial.printf("Firmware %s\n", FW_VERSION);
  Serial.println(SensorProtocol::selfTest()
                     ? "Protocol self-test: OK"
                     : "Protocol self-test: FAILED");
  Serial.println("UART2 joystick link: RX=16 TX=17");
  u8g2.begin();
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  if (!MDNS.begin("esp32-b")) {
    Serial.println("mDNS begin failed");
  } else {
    // Advertise TCP service so A can resolve esp32-b.local more reliably
    MDNS.addService("esp32b", "tcp", 8080);
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: esp32-b.local");
  }
  server.begin();
  displayQueue = xQueueCreate(20, sizeof(DisplayMsg));

  // Arduino-ESP32 may initialize TWDT before setup(). Reconfigure it when
  // already active; initialize only when it does not exist.
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = 5000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  const esp_err_t setupTaskStatus = esp_task_wdt_status(NULL);
  const esp_err_t watchdogConfigResult =
      setupTaskStatus == ESP_ERR_INVALID_STATE
          ? esp_task_wdt_init(&twdt_config)
          : esp_task_wdt_reconfigure(&twdt_config);
  //#region agent log
  if (watchdogConfigResult != ESP_OK) {
    Serial.printf("Watchdog configuration failed: 0x%x\n",
                  watchdogConfigResult);
  }
  //#endregion

  // If Arduino subscribed loopTask, unsubscribe before deleting it below.
  if (setupTaskStatus == ESP_OK) {
    esp_task_wdt_delete(NULL);
  }

  // Equal priority lets FreeRTOS time-slice networking and display work.
  xTaskCreate(ServerTask, "Serv", 4096, NULL, 2, NULL);
  xTaskCreate(UartLinkTask, "Uart", 4096, NULL, 2, NULL);
  xTaskCreate(DisplayTask, "Disp", 8192, NULL, 2, NULL);
  xTaskCreate(OtaTask, "OTA", 8192, NULL, 1, NULL);
  vTaskDelete(NULL);
}

void loop() {}