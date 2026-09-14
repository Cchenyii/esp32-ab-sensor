#include "protocol.h"

#include <math.h>
#include <string.h>

namespace SensorProtocol {
namespace {

uint16_t readU16Be(const uint8_t* data) {
  return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

uint32_t readU32Be(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

void writeU16Be(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value >> 8);
  output[1] = static_cast<uint8_t>(value);
}

void writeU32Be(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24);
  output[1] = static_cast<uint8_t>(value >> 16);
  output[2] = static_cast<uint8_t>(value >> 8);
  output[3] = static_cast<uint8_t>(value);
}

int32_t clampValue(int32_t value, int32_t minimum, int32_t maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

}  // namespace

uint16_t crc16Ccitt(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

size_t encodeFrame(uint8_t type, uint32_t sequence,
                   const uint8_t* payload, uint16_t payloadLength,
                   uint8_t* output, size_t outputCapacity) {
  if (output == nullptr || payloadLength > MAX_PAYLOAD_SIZE ||
      (payloadLength > 0 && payload == nullptr)) {
    return 0;
  }

  const size_t frameLength = FIXED_HEADER_SIZE + payloadLength + 2;
  if (outputCapacity < frameLength) return 0;

  output[0] = MAGIC_1;
  output[1] = MAGIC_2;
  output[2] = VERSION;
  output[3] = type;
  writeU32Be(output + 4, sequence);
  writeU16Be(output + 8, payloadLength);
  if (payloadLength > 0) {
    memcpy(output + FIXED_HEADER_SIZE, payload, payloadLength);
  }

  const uint16_t crc =
      crc16Ccitt(output + 2, FIXED_HEADER_SIZE - 2 + payloadLength);
  writeU16Be(output + FIXED_HEADER_SIZE + payloadLength, crc);
  return frameLength;
}

size_t encodeSensorFrame(uint32_t sequence, float temperatureC,
                         float humidityPercent, int distanceCm,
                         uint8_t* output, size_t outputCapacity) {
  int32_t temperature = lroundf(temperatureC * 100.0f);
  int32_t humidity = lroundf(humidityPercent * 100.0f);
  temperature = clampValue(temperature, INT16_MIN, INT16_MAX);
  humidity = clampValue(humidity, 0, 10000);
  distanceCm = clampValue(distanceCm, 0, UINT16_MAX);

  uint8_t payload[6];
  writeU16Be(payload, static_cast<uint16_t>(
                          static_cast<int16_t>(temperature)));
  writeU16Be(payload + 2, static_cast<uint16_t>(humidity));
  writeU16Be(payload + 4, static_cast<uint16_t>(distanceCm));
  return encodeFrame(SENSOR_DATA, sequence, payload, sizeof(payload),
                     output, outputCapacity);
}

bool decodeSensorPayload(const Frame& frame, SensorPayload& output) {
  if (frame.type != SENSOR_DATA || frame.payloadLength != 6) return false;

  output.temperatureCentiC =
      static_cast<int16_t>(readU16Be(frame.payload));
  output.humidityCentiPercent = readU16Be(frame.payload + 2);
  output.distanceCm = readU16Be(frame.payload + 4);
  return true;
}

size_t encodeJoystickFrame(uint32_t sequence, uint16_t x, uint16_t y,
                           uint8_t direction, uint8_t sw, uint8_t* output,
                           size_t outputCapacity) {
  uint8_t payload[6];
  writeU16Be(payload, x);
  writeU16Be(payload + 2, y);
  payload[4] = direction;
  payload[5] = sw ? 1 : 0;
  return encodeFrame(JOYSTICK_DATA, sequence, payload, sizeof(payload),
                     output, outputCapacity);
}

bool decodeJoystickPayload(const Frame& frame, JoystickPayload& output) {
  if (frame.type != JOYSTICK_DATA || frame.payloadLength != 6) return false;
  output.x = readU16Be(frame.payload);
  output.y = readU16Be(frame.payload + 2);
  output.direction = frame.payload[4];
  output.sw = frame.payload[5];
  return true;
}

RingBuffer::RingBuffer() : head_(0), tail_(0), size_(0) {}

bool RingBuffer::push(uint8_t value) {
  bool noOverflow = true;
  if (size_ == RING_CAPACITY) {
    uint8_t ignored;
    pop(ignored);
    noOverflow = false;
  }
  data_[tail_] = value;
  tail_ = (tail_ + 1) % RING_CAPACITY;
  ++size_;
  return noOverflow;
}

bool RingBuffer::push(const uint8_t* data, size_t length) {
  if (data == nullptr && length > 0) return false;
  bool noOverflow = true;
  for (size_t i = 0; i < length; ++i) {
    if (!push(data[i])) noOverflow = false;
  }
  return noOverflow;
}

bool RingBuffer::peek(size_t offset, uint8_t& value) const {
  if (offset >= size_) return false;
  value = data_[(head_ + offset) % RING_CAPACITY];
  return true;
}

bool RingBuffer::pop(uint8_t& value) {
  if (size_ == 0) return false;
  value = data_[head_];
  head_ = (head_ + 1) % RING_CAPACITY;
  --size_;
  return true;
}

void RingBuffer::discard(size_t count) {
  if (count > size_) count = size_;
  head_ = (head_ + count) % RING_CAPACITY;
  size_ -= count;
}

size_t RingBuffer::size() const {
  return size_;
}

void RingBuffer::clear() {
  head_ = 0;
  tail_ = 0;
  size_ = 0;
}

FrameParser::FrameParser() : overflowed_(false) {}

ParseResult FrameParser::push(const uint8_t* data, size_t length) {
  if (!buffer_.push(data, length)) {
    overflowed_ = true;
    return ParseResult::BUFFER_OVERFLOW;
  }
  return ParseResult::NONE;
}

ParseResult FrameParser::next(Frame& frame) {
  if (overflowed_) {
    overflowed_ = false;
    return ParseResult::BUFFER_OVERFLOW;
  }

  uint8_t first = 0;
  uint8_t second = 0;
  while (buffer_.size() >= 2) {
    buffer_.peek(0, first);
    buffer_.peek(1, second);
    if (first == MAGIC_1 && second == MAGIC_2) break;
    buffer_.discard(1);
  }

  if (buffer_.size() < FIXED_HEADER_SIZE) return ParseResult::NONE;

  uint8_t header[FIXED_HEADER_SIZE];
  for (size_t i = 0; i < FIXED_HEADER_SIZE; ++i) {
    buffer_.peek(i, header[i]);
  }

  const uint16_t payloadLength = readU16Be(header + 8);
  if (payloadLength > MAX_PAYLOAD_SIZE) {
    buffer_.discard(1);
    return ParseResult::LENGTH_ERROR;
  }

  const size_t frameLength = FIXED_HEADER_SIZE + payloadLength + 2;
  if (buffer_.size() < frameLength) return ParseResult::NONE;

  uint8_t crcData[FIXED_HEADER_SIZE - 2 + MAX_PAYLOAD_SIZE];
  for (size_t i = 0; i < FIXED_HEADER_SIZE - 2 + payloadLength; ++i) {
    buffer_.peek(i + 2, crcData[i]);
  }

  uint8_t crcHigh = 0;
  uint8_t crcLow = 0;
  buffer_.peek(FIXED_HEADER_SIZE + payloadLength, crcHigh);
  buffer_.peek(FIXED_HEADER_SIZE + payloadLength + 1, crcLow);
  const uint16_t expectedCrc =
      (static_cast<uint16_t>(crcHigh) << 8) | crcLow;
  const uint16_t actualCrc =
      crc16Ccitt(crcData, FIXED_HEADER_SIZE - 2 + payloadLength);
  if (actualCrc != expectedCrc) {
    buffer_.discard(1);
    return ParseResult::CRC_ERROR;
  }

  if (header[2] != VERSION) {
    buffer_.discard(frameLength);
    return ParseResult::VERSION_ERROR;
  }

  frame.version = header[2];
  frame.type = header[3];
  frame.sequence = readU32Be(header + 4);
  frame.payloadLength = payloadLength;
  for (size_t i = 0; i < payloadLength; ++i) {
    buffer_.peek(FIXED_HEADER_SIZE + i, frame.payload[i]);
  }
  buffer_.discard(frameLength);
  return ParseResult::FRAME_READY;
}

void FrameParser::reset() {
  buffer_.clear();
  overflowed_ = false;
}

size_t FrameParser::bufferedBytes() const {
  return buffer_.size();
}

bool selfTest() {
  static const uint8_t check[] = {'1', '2', '3', '4', '5',
                                  '6', '7', '8', '9'};
  if (crc16Ccitt(check, sizeof(check)) != 0x29B1) return false;

  uint8_t sensorFrame[MAX_FRAME_SIZE];
  const size_t sensorLength =
      encodeSensorFrame(0x01020304, 27.35f, 48.50f, 14,
                        sensorFrame, sizeof(sensorFrame));
  if (sensorLength != 18) return false;

  FrameParser parser;
  Frame decoded;
  if (parser.push(sensorFrame, 5) != ParseResult::NONE) return false;
  if (parser.next(decoded) != ParseResult::NONE) return false;
  if (parser.push(sensorFrame + 5, sensorLength - 5) != ParseResult::NONE) {
    return false;
  }
  if (parser.next(decoded) != ParseResult::FRAME_READY) return false;
  if (decoded.sequence != 0x01020304) return false;

  SensorPayload sensor;
  if (!decodeSensorPayload(decoded, sensor)) return false;
  if (sensor.temperatureCentiC != 2735 ||
      sensor.humidityCentiPercent != 4850 ||
      sensor.distanceCm != 14) {
    return false;
  }

  uint8_t ackPayload[] = {ACK_OK};
  uint8_t ackFrame[MAX_FRAME_SIZE];
  const size_t ackLength =
      encodeFrame(ACK, 0x01020304, ackPayload, sizeof(ackPayload),
                  ackFrame, sizeof(ackFrame));
  uint8_t sticky[MAX_FRAME_SIZE * 2];
  memcpy(sticky, sensorFrame, sensorLength);
  memcpy(sticky + sensorLength, ackFrame, ackLength);

  parser.reset();
  parser.push(sticky, sensorLength + ackLength);
  if (parser.next(decoded) != ParseResult::FRAME_READY ||
      decoded.type != SENSOR_DATA) {
    return false;
  }
  if (parser.next(decoded) != ParseResult::FRAME_READY ||
      decoded.type != ACK) {
    return false;
  }

  uint8_t corrupted[MAX_FRAME_SIZE];
  memcpy(corrupted, sensorFrame, sensorLength);
  corrupted[FIXED_HEADER_SIZE] ^= 0x01;
  parser.reset();
  parser.push(corrupted, sensorLength);
  if (parser.next(decoded) != ParseResult::CRC_ERROR) return false;

  return true;
}

}  // namespace SensorProtocol
