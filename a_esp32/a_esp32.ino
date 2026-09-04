// Board A: sensor acquisition and TCP client

#include <WiFi.h>
#include <DHT.h>
#include <NewPing.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include "protocol.h"
#include "secrets.h"

// NetworkClient (Arduino-ESP32 3.x) has no keepAlive(); use setsockopt instead.
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

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* serverHostname = "esp32-b.local";
const int serverPort = 8080;

#define DHTPIN 4
#define DHTTYPE DHT22
#define TRIGGER_PIN 5
#define ECHO_PIN 18
#define MAX_DISTANCE 200

typedef struct {
  float temp, humi;
  int dist;
} SensorData;

DHT dht(DHTPIN, DHTTYPE);
NewPing sonar(TRIGGER_PIN, ECHO_PIN, MAX_DISTANCE);
WiFiClient client;
QueueHandle_t sensorQueue;
IPAddress cachedIP;

void SensorTask(void* pv) {
  SensorData data;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t interval = pdMS_TO_TICKS(2000);
  while (1) {
    data.temp = dht.readTemperature();
    data.humi = dht.readHumidity();
    data.dist = sonar.ping_cm();
    if (data.dist == 0) data.dist = 200;
    if (isnan(data.temp) || isnan(data.humi)) {
      vTaskDelayUntil(&lastWake, interval);
      continue;
    }
    if (xQueueSend(sensorQueue, &data, 0) != pdTRUE) {
      xQueueReset(sensorQueue);
      xQueueSend(sensorQueue, &data, 0);
    }
    vTaskDelayUntil(&lastWake, interval);
  }
}

void TCPTask(void* pv) {
  SensorData latestData;
  bool hasLatestData = false;
  TickType_t lastAttempt = 0;
  const TickType_t retryDelay = pdMS_TO_TICKS(3000);
  uint32_t sequence = 0;
  SensorProtocol::FrameParser ackParser;
  SensorProtocol::Frame ackFrame;

  uint8_t pendingFrame[SensorProtocol::MAX_FRAME_SIZE];
  size_t pendingLength = 0;
  uint32_t pendingSequence = 0;
  uint32_t pendingSentAt = 0;
  uint8_t retryCount = 0;
  bool hasPendingFrame = false;

  uint32_t framesCreated = 0;
  uint32_t framesAcknowledged = 0;
  uint32_t retransmissions = 0;
  uint32_t acknowledgementTimeouts = 0;
  uint32_t acknowledgementErrors = 0;
  uint32_t reconnects = 0;
  uint32_t lastStatsPrint = millis();

  while (1) {
    if (WiFi.status() != WL_CONNECTED) {
      client.stop();
      ackParser.reset();
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      int retries = 0;
      while (WiFi.status() != WL_CONNECTED && retries < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retries++;
      }
      continue;
    }

    if (!client.connected()) {
      if ((xTaskGetTickCount() - lastAttempt) > retryDelay) {
        lastAttempt = xTaskGetTickCount();
        IPAddress ip;

        // --- mDNS primary discovery (project feature) ---
        bool ipValid = false;
        const char* ipSource = "none";
        for (int tryN = 0; tryN < 3 && !ipValid; tryN++) {
          IPAddress mdnsIp;
          bool mdnsOk = WiFi.hostByName(serverHostname, mdnsIp);
          ipValid = mdnsOk && mdnsIp != IPAddress(0, 0, 0, 0);
          if (ipValid) {
            ip = mdnsIp;
            cachedIP = mdnsIp;
            ipSource = "mDNS";
          } else if (tryN < 2) {
            vTaskDelay(pdMS_TO_TICKS(300));
          }
        }

        if (!ipValid && cachedIP != IPAddress(0, 0, 0, 0)) {
          ip = cachedIP;
          ipValid = true;
          ipSource = "cache";
        }

        if (!ipValid && SERVER_IP_FALLBACK != nullptr &&
            SERVER_IP_FALLBACK[0] != '\0' &&
            ip.fromString(SERVER_IP_FALLBACK)) {
          ipValid = true;
          ipSource = "fallback";
        }

        if (!ipValid) {
          Serial.println("B not found (mDNS/cache/fallback all failed)");
          vTaskDelay(pdMS_TO_TICKS(1000));
          continue;
        }

        Serial.printf("Connecting to %s via %s ...\n",
                      ip.toString().c_str(), ipSource);
        bool connOk = client.connect(ip, serverPort);
        if (connOk) {
          client.setTimeout(50);
          client.setNoDelay(true);
          enableTcpKeepAlive(client);
          vTaskDelay(pdMS_TO_TICKS(100));
          ackParser.reset();
          reconnects++;
          Serial.println("Connected");

          // Preserve and resend an unacknowledged frame across reconnects.
          if (hasPendingFrame) {
            retryCount = 0;
            if (writeCompleteFrame(client, pendingFrame, pendingLength)) {
              pendingSentAt = millis();
              retransmissions++;
            } else {
              client.stop();
            }
          }
        } else {
          Serial.println("Connect failed");
        }
      }
    }

    // ACK receive path is non-blocking; TCPTask continues servicing sensors.
    uint8_t receiveBuffer[64];
    while (client.connected() && client.available()) {
      const size_t requested =
          min(static_cast<size_t>(client.available()), sizeof(receiveBuffer));
      const int received = client.read(receiveBuffer, requested);
      if (received <= 0) break;

      if (ackParser.push(receiveBuffer, received) ==
          SensorProtocol::ParseResult::BUFFER_OVERFLOW) {
        acknowledgementErrors++;
      }

      while (true) {
        const SensorProtocol::ParseResult result = ackParser.next(ackFrame);
        if (result == SensorProtocol::ParseResult::NONE) break;
        if (result != SensorProtocol::ParseResult::FRAME_READY) {
          acknowledgementErrors++;
          continue;
        }
        if (ackFrame.type == SensorProtocol::ACK &&
            ackFrame.payloadLength == 1 && hasPendingFrame &&
            ackFrame.sequence == pendingSequence &&
            ackFrame.payload[0] == SensorProtocol::ACK_OK) {
          hasPendingFrame = false;
          pendingLength = 0;
          retryCount = 0;
          framesAcknowledged++;
        } else {
          acknowledgementErrors++;
        }
      }
    }

    // Always retain only the newest sample while another frame awaits ACK.
    SensorData queuedData;
    while (xQueueReceive(sensorQueue, &queuedData, 0) == pdTRUE) {
      latestData = queuedData;
      hasLatestData = true;
    }

    if (client.connected() && !hasPendingFrame && hasLatestData) {
      pendingSequence = ++sequence;
      pendingLength = SensorProtocol::encodeSensorFrame(
          pendingSequence, latestData.temp, latestData.humi, latestData.dist,
          pendingFrame, sizeof(pendingFrame));
      hasLatestData = false;

      if (pendingLength == 0) {
        Serial.println("Frame encode failed");
      } else {
        hasPendingFrame = true;
        retryCount = 0;
        framesCreated++;
        if (writeCompleteFrame(client, pendingFrame, pendingLength)) {
          pendingSentAt = millis();
        } else {
          pendingSentAt = 0;
          client.stop();
          Serial.println("Frame send failed, reconnect");
        }
      }
    }

    if (client.connected() && hasPendingFrame &&
        millis() - pendingSentAt >= 1200) {
      if (retryCount < 2) {
        retryCount++;
        retransmissions++;
        if (writeCompleteFrame(client, pendingFrame, pendingLength)) {
          pendingSentAt = millis();
        } else {
          pendingSentAt = 0;
          client.stop();
        }
      } else {
        acknowledgementTimeouts++;
        client.stop();
        ackParser.reset();
        retryCount = 0;
        pendingSentAt = 0;
        Serial.println("ACK timeout, reconnect");
      }
    }

    if (millis() - lastStatsPrint >= 60000) {
      lastStatsPrint = millis();
      Serial.printf(
          "ACK created=%lu acknowledged=%lu retransmitted=%lu "
          "timeouts=%lu errors=%lu reconnects=%lu pending=%d\n",
          (unsigned long)framesCreated,
          (unsigned long)framesAcknowledged,
          (unsigned long)retransmissions,
          (unsigned long)acknowledgementTimeouts,
          (unsigned long)acknowledgementErrors,
          (unsigned long)reconnects, hasPendingFrame ? 1 : 0);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println(SensorProtocol::selfTest()
                     ? "Protocol self-test: OK"
                     : "Protocol self-test: FAILED");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));
  dht.begin();
  cachedIP = IPAddress(0, 0, 0, 0);  // filled only after a successful mDNS resolve
  sensorQueue = xQueueCreate(5, sizeof(SensorData));
  xTaskCreate(SensorTask, "Sens", 4096, NULL, 1, NULL);
  xTaskCreate(TCPTask, "TCP", 4096, NULL, 2, NULL);
  vTaskDelete(NULL);
}

void loop() {}
