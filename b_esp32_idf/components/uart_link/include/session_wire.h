#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* V1 envelope and big-endian fields remain unchanged. */
enum {
    SW_REQUEST = 0x05, SW_OFFER = 0x06, SW_CONFIRM = 0x07,
    SW_JOYSTICK = 0x08, SW_ACK = 0x09,
    SW_SENSOR = 0x0a, SW_SENSOR_ACK = 0x0b,
    SW_OK = 0, SW_BAD_LENGTH = 2, SW_BAD_TYPE = 4, SW_NEED_SESSION = 5,
    SW_MAX_PAYLOAD = 64, SW_MAX_FRAME = 76
};
enum {
    SW_SENSOR_VALID = 1, SW_WIFI_READY = 2, SW_TCP_CONNECTED = 4, SW_SENSOR_SEEN = 8,
    SW_SENSOR_PAYLOAD_SIZE = 27
};
typedef struct {
    uint64_t token;
    uint32_t source_connection, source_sequence, valid_for_ms;
    int16_t temperature_centi_c;
    uint16_t humidity_centi_percent, distance_cm;
    uint8_t flags;
} sw_sensor_t;
typedef struct {
    uint8_t type;
    uint32_t sequence;
    uint16_t length;
    uint8_t payload[SW_MAX_PAYLOAD];
} sw_frame_t;
typedef struct {
    uint8_t data[SW_MAX_FRAME];
    size_t used;
    uint32_t rejected;
} sw_parser_t;
uint16_t sw_crc(const uint8_t *data, size_t size);
uint64_t sw_read_token(const uint8_t *data);
void sw_write_token(uint8_t *data, uint64_t token);
size_t sw_encode(uint8_t type, uint32_t sequence, const uint8_t *payload,
                 uint16_t length, uint8_t *output, size_t capacity);
size_t sw_encode_sensor(uint32_t relay_sequence, const sw_sensor_t *sensor,
                        uint8_t *output, size_t capacity);
int sw_decode_sensor(const sw_frame_t *frame, sw_sensor_t *sensor);
/* Feed one byte and consume a complete valid frame. Noise, CRC and length
 * failures resynchronize inside the same call; partial frames are retained. */
int sw_parser_feed(sw_parser_t *parser, uint8_t byte, sw_frame_t *frame);
int sw_wire_self_test(void);
#ifdef __cplusplus
}
#endif
