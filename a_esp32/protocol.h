#pragma once

#include <Arduino.h>

namespace SensorProtocol {

constexpr uint8_t MAGIC_1 = 0xAA;
constexpr uint8_t MAGIC_2 = 0x55;
constexpr uint8_t VERSION = 0x01;
constexpr size_t FIXED_HEADER_SIZE = 10;
constexpr size_t MAX_PAYLOAD_SIZE = 64;
constexpr size_t MAX_FRAME_SIZE = FIXED_HEADER_SIZE + MAX_PAYLOAD_SIZE + 2;
constexpr size_t RING_CAPACITY = 256;

enum MessageType : uint8_t {
  SENSOR_DATA = 0x01,
  ACK = 0x02,
  HEARTBEAT = 0x03,
  JOYSTICK_DATA = 0x04,  // STM32 UART link (same V1 envelope)
  OTA_STATUS = 0x10,
};

enum JoyDirection : uint8_t {
  JOY_CENTER = 0,
  JOY_LEFT = 1,
  JOY_RIGHT = 2,
  JOY_UP = 3,
  JOY_DOWN = 4,
};

enum AckStatus : uint8_t {
  ACK_OK = 0x00,
  ACK_CRC_ERROR = 0x01,
  ACK_LENGTH_ERROR = 0x02,
  ACK_VERSION_ERROR = 0x03,
  ACK_TYPE_ERROR = 0x04,
};

enum class ParseResult : uint8_t {
  NONE,
  FRAME_READY,
  CRC_ERROR,
  LENGTH_ERROR,
  VERSION_ERROR,
  BUFFER_OVERFLOW,
};

struct Frame {
  uint8_t version;
  uint8_t type;
  uint32_t sequence;
  uint16_t payloadLength;
  uint8_t payload[MAX_PAYLOAD_SIZE];
};

struct SensorPayload {
  int16_t temperatureCentiC;
  uint16_t humidityCentiPercent;
  uint16_t distanceCm;
};

struct JoystickPayload {
  uint16_t x;
  uint16_t y;
  uint8_t direction;  // JoyDirection
  uint8_t sw;         // 1=pressed
};

uint16_t crc16Ccitt(const uint8_t* data, size_t length);

size_t encodeFrame(uint8_t type, uint32_t sequence,
                   const uint8_t* payload, uint16_t payloadLength,
                   uint8_t* output, size_t outputCapacity);

size_t encodeSensorFrame(uint32_t sequence, float temperatureC,
                         float humidityPercent, int distanceCm,
                         uint8_t* output, size_t outputCapacity);

bool decodeSensorPayload(const Frame& frame, SensorPayload& output);

size_t encodeJoystickFrame(uint32_t sequence, uint16_t x, uint16_t y,
                           uint8_t direction, uint8_t sw, uint8_t* output,
                           size_t outputCapacity);

bool decodeJoystickPayload(const Frame& frame, JoystickPayload& output);

class RingBuffer {
 public:
  RingBuffer();

  bool push(uint8_t value);
  bool push(const uint8_t* data, size_t length);
  bool peek(size_t offset, uint8_t& value) const;
  bool pop(uint8_t& value);
  void discard(size_t count);
  size_t size() const;
  void clear();

 private:
  uint8_t data_[RING_CAPACITY];
  size_t head_;
  size_t tail_;
  size_t size_;
};

class FrameParser {
 public:
  FrameParser();

  ParseResult push(const uint8_t* data, size_t length);
  ParseResult next(Frame& frame);
  void reset();
  size_t bufferedBytes() const;

 private:
  RingBuffer buffer_;
  bool overflowed_;
};

// Startup verification for CRC, split frames, sticky frames and bad CRC.
bool selfTest();

}  // namespace SensorProtocol
