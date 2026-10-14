#include "protocol_self_test.h"
#include "protocol.h"

#include <string.h>

namespace {
using namespace SensorProtocol;

// Fixed V1 bytes, including independent CRC-16/CCITT-FALSE reference values.
// Sequence 0x01020304; sensor: 27.35 C, 48.50 %, 14 cm;
// joystick: X=2048, Y=2050, CENTER, pressed; ACK status: OK.
constexpr uint8_t SENSOR_WIRE[] = {
    0xAA, 0x55, 0x01, 0x01, 0x01, 0x02, 0x03, 0x04, 0x00,
    0x06, 0x0A, 0xAF, 0x12, 0xF2, 0x00, 0x0E, 0xEB, 0x07,
};
constexpr uint8_t JOYSTICK_WIRE[] = {
    0xAA, 0x55, 0x01, 0x04, 0x01, 0x02, 0x03, 0x04, 0x00,
    0x06, 0x08, 0x00, 0x08, 0x02, 0x00, 0x01, 0x0E, 0xA5,
};
constexpr uint8_t ACK_WIRE[] = {
    0xAA, 0x55, 0x01, 0x02, 0x01, 0x02, 0x03,
    0x04, 0x00, 0x01, 0x00, 0x58, 0xC4,
};

struct WireFrame {
    const uint8_t *bytes;
    size_t length;
};

constexpr WireFrame FRAMES[] = {
    {SENSOR_WIRE, sizeof(SENSOR_WIRE)},
    {JOYSTICK_WIRE, sizeof(JOYSTICK_WIRE)},
    {ACK_WIRE, sizeof(ACK_WIRE)},
};

bool matches(const Frame &frame, const WireFrame &wire)
{
    const uint16_t length =
        (static_cast<uint16_t>(wire.bytes[8]) << 8) | wire.bytes[9];
    return frame.version == VERSION && frame.type == wire.bytes[3] &&
           frame.sequence == 0x01020304 && frame.payloadLength == length &&
           memcmp(frame.payload, wire.bytes + FIXED_HEADER_SIZE, length) == 0;
}

bool crc_known_vector()
{
    static const uint8_t input[] = "123456789";
    return crc16Ccitt(input, sizeof(input) - 1) == 0x29B1 &&
           crc16Ccitt(nullptr, 0) == 0xFFFF;
}

bool golden_wire_vectors()
{
    uint8_t output[MAX_FRAME_SIZE];
    if (encodeSensorFrame(0x01020304, 27.35f, 48.50f, 14,
                          output, sizeof(output)) != sizeof(SENSOR_WIRE) ||
        memcmp(output, SENSOR_WIRE, sizeof(SENSOR_WIRE)) != 0) return false;
    if (encodeJoystickFrame(0x01020304, 2048, 2050, JOY_CENTER, 1,
                            output, sizeof(output)) != sizeof(JOYSTICK_WIRE) ||
        memcmp(output, JOYSTICK_WIRE, sizeof(JOYSTICK_WIRE)) != 0) return false;
    const uint8_t status = ACK_OK;
    if (encodeFrame(ACK, 0x01020304, &status, 1, output, sizeof(output)) !=
            sizeof(ACK_WIRE) ||
        memcmp(output, ACK_WIRE, sizeof(ACK_WIRE)) != 0) return false;

    FrameParser parser;
    Frame frame;
    parser.push(JOYSTICK_WIRE, sizeof(JOYSTICK_WIRE));
    JoystickPayload joystick;
    if (parser.next(frame) != ParseResult::FRAME_READY ||
        !decodeJoystickPayload(frame, joystick)) return false;
    return joystick.x == 2048 && joystick.y == 2050 &&
           joystick.direction == JOY_CENTER && joystick.sw == 1;
}

bool every_split_boundary()
{
    for (const WireFrame &wire : FRAMES) {
        for (size_t split = 1; split < wire.length; ++split) {
            FrameParser parser;
            Frame frame;
            if (parser.push(wire.bytes, split) != ParseResult::NONE ||
                parser.next(frame) != ParseResult::NONE ||
                parser.bufferedBytes() != split) return false;
            if (parser.push(wire.bytes + split, wire.length - split) !=
                    ParseResult::NONE ||
                parser.next(frame) != ParseResult::FRAME_READY ||
                !matches(frame, wire) ||
                parser.next(frame) != ParseResult::NONE ||
                parser.bufferedBytes() != 0) return false;
        }

        FrameParser parser;
        Frame frame;
        for (size_t index = 0; index < wire.length; ++index) {
            if (parser.push(wire.bytes + index, 1) != ParseResult::NONE) return false;
            const ParseResult expected = index + 1 == wire.length
                ? ParseResult::FRAME_READY : ParseResult::NONE;
            if (parser.next(frame) != expected) return false;
        }
        if (!matches(frame, wire) || parser.bufferedBytes() != 0) return false;
    }
    return true;
}

bool sticky_frames()
{
    uint8_t stream[sizeof(SENSOR_WIRE) + sizeof(JOYSTICK_WIRE) + sizeof(ACK_WIRE)];
    size_t offset = 0;
    for (const WireFrame &wire : FRAMES) {
        memcpy(stream + offset, wire.bytes, wire.length);
        offset += wire.length;
    }

    FrameParser parser;
    Frame frame;
    if (parser.push(stream, sizeof(stream)) != ParseResult::NONE) return false;
    for (const WireFrame &wire : FRAMES) {
        if (parser.next(frame) != ParseResult::FRAME_READY ||
            !matches(frame, wire)) return false;
    }
    return parser.next(frame) == ParseResult::NONE && parser.bufferedBytes() == 0;
}

bool bad_crc_recovery()
{
    uint8_t stream[sizeof(SENSOR_WIRE) + sizeof(ACK_WIRE)];
    memcpy(stream, SENSOR_WIRE, sizeof(SENSOR_WIRE));
    stream[FIXED_HEADER_SIZE] ^= 0x01;
    memcpy(stream + sizeof(SENSOR_WIRE), ACK_WIRE, sizeof(ACK_WIRE));

    FrameParser parser;
    Frame frame;
    if (parser.push(stream, sizeof(stream)) != ParseResult::NONE ||
        parser.next(frame) != ParseResult::CRC_ERROR ||
        parser.next(frame) != ParseResult::FRAME_READY ||
        !matches(frame, FRAMES[2])) return false;
    return parser.next(frame) == ParseResult::NONE && parser.bufferedBytes() == 0;
}

bool noise_resynchronization()
{
    static const uint8_t noise[] = {0x00, 0x55, 0xAA, 0x00, 0xFF, 0xAA};
    FrameParser parser;
    Frame frame;
    if (parser.push(noise, sizeof(noise)) != ParseResult::NONE ||
        parser.push(SENSOR_WIRE, sizeof(SENSOR_WIRE)) != ParseResult::NONE ||
        parser.next(frame) != ParseResult::FRAME_READY ||
        !matches(frame, FRAMES[0])) return false;
    return parser.next(frame) == ParseResult::NONE && parser.bufferedBytes() == 0;
}

bool ring_buffer_wrap()
{
    FrameParser parser;
    Frame frame;
    // 882 bytes through a 256-byte ring, consuming frames between writes.
    for (size_t round = 0; round < 18; ++round) {
        for (const WireFrame &wire : FRAMES) {
            if (parser.push(wire.bytes, wire.length) != ParseResult::NONE ||
                parser.next(frame) != ParseResult::FRAME_READY ||
                !matches(frame, wire) || parser.bufferedBytes() != 0) return false;
        }
    }
    return true;
}

struct TestCase {
    const char *name;
    bool (*run)();
};

constexpr TestCase TESTS[] = {
    {"legacy", SensorProtocol::selfTest},
    {"crc_known_vector", crc_known_vector},
    {"golden_wire_vectors", golden_wire_vectors},
    {"every_split_boundary", every_split_boundary},
    {"sticky_frames", sticky_frames},
    {"bad_crc_recovery", bad_crc_recovery},
    {"noise_resynchronization", noise_resynchronization},
    {"ring_buffer_wrap", ring_buffer_wrap},
};

static_assert(sizeof(TESTS) / sizeof(TESTS[0]) == SENSOR_PROTOCOL_SELF_TEST_COUNT,
              "Self-test report size must match the test cases");
}  // namespace

extern "C" bool sensor_protocol_run_self_tests(sensor_protocol_self_test_report_t *report)
{
    if (report == nullptr) return false;
    report->count = SENSOR_PROTOCOL_SELF_TEST_COUNT;
    bool all_passed = true;
    for (size_t index = 0; index < report->count; ++index) {
        report->results[index].name = TESTS[index].name;
        report->results[index].passed = TESTS[index].run();
        all_passed = report->results[index].passed && all_passed;
    }
    return all_passed;
}
