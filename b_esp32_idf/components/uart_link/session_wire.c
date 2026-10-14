#include "session_wire.h"
#include <string.h>

static uint16_t read16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t read32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void write16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}
static void write32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}
uint16_t sw_crc(const uint8_t *data, size_t size) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
uint64_t sw_read_token(const uint8_t *p) {
    return ((uint64_t)read32(p) << 32) | read32(p + 4);
}
void sw_write_token(uint8_t *p, uint64_t token) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(token >> (56 - 8 * i));
}
size_t sw_encode(uint8_t type, uint32_t sequence, const uint8_t *payload,
                 uint16_t length, uint8_t *out, size_t capacity) {
    size_t size = 12u + length;
    if (!out || length > SW_MAX_PAYLOAD || capacity < size || (length && !payload)) return 0;
    out[0] = 0xaa; out[1] = 0x55; out[2] = 1; out[3] = type;
    out[4] = (uint8_t)(sequence >> 24); out[5] = (uint8_t)(sequence >> 16);
    out[6] = (uint8_t)(sequence >> 8); out[7] = (uint8_t)sequence;
    out[8] = (uint8_t)(length >> 8); out[9] = (uint8_t)length;
    if (length) memcpy(out + 10, payload, length);
    uint16_t crc = sw_crc(out + 2, 8u + length);
    out[10 + length] = (uint8_t)(crc >> 8); out[11 + length] = (uint8_t)crc;
    return size;
}
size_t sw_encode_sensor(uint32_t relay_sequence, const sw_sensor_t *s,
                        uint8_t *output, size_t capacity) {
    if (!s) return 0;
    uint8_t p[SW_SENSOR_PAYLOAD_SIZE];
    sw_write_token(p, s->token);
    write32(p + 8, s->source_connection); write32(p + 12, s->source_sequence);
    write32(p + 16, s->valid_for_ms);
    write16(p + 20, (uint16_t)s->temperature_centi_c);
    write16(p + 22, s->humidity_centi_percent); write16(p + 24, s->distance_cm);
    p[26] = s->flags;
    return sw_encode(SW_SENSOR, relay_sequence, p, sizeof(p), output, capacity);
}
int sw_decode_sensor(const sw_frame_t *f, sw_sensor_t *s) {
    if (!f || !s || f->type != SW_SENSOR || f->length != SW_SENSOR_PAYLOAD_SIZE) return 0;
    s->token = sw_read_token(f->payload);
    s->source_connection = read32(f->payload + 8);
    s->source_sequence = read32(f->payload + 12);
    s->valid_for_ms = read32(f->payload + 16);
    /* Arithmetic conversion makes negative centi-degrees explicit. */
    uint16_t raw_temp = read16(f->payload + 20);
    s->temperature_centi_c = (int16_t)(raw_temp < 0x8000u ? (int32_t)raw_temp : (int32_t)raw_temp - 65536);
    s->humidity_centi_percent = read16(f->payload + 22);
    s->distance_cm = read16(f->payload + 24); s->flags = f->payload[26];
    if (!s->token || s->flags & ~15u || s->humidity_centi_percent > 10000u ||
        s->valid_for_ms > 60000u) return 0;
    if (s->flags & SW_SENSOR_VALID) {
        return s->valid_for_ms > 0 &&
               (s->flags & (SW_WIFI_READY | SW_TCP_CONNECTED | SW_SENSOR_SEEN)) ==
               (SW_WIFI_READY | SW_TCP_CONNECTED | SW_SENSOR_SEEN);
    }
    return s->valid_for_ms == 0;
}
static void discard(sw_parser_t *p, size_t n) {
    p->used -= n; memmove(p->data, p->data + n, p->used);
}
int sw_parser_feed(sw_parser_t *p, uint8_t byte, sw_frame_t *frame) {
    if (p->used == sizeof(p->data)) { discard(p, 1); ++p->rejected; }
    p->data[p->used++] = byte;
    while (p->used >= 2) {
        if (p->data[0] != 0xaa || p->data[1] != 0x55) { discard(p, 1); continue; }
        if (p->used < 10) return 0;
        uint16_t length = read16(p->data + 8);
        if (length > SW_MAX_PAYLOAD || p->data[2] != 1) { discard(p, 1); ++p->rejected; continue; }
        size_t size = 12u + length;
        if (p->used < size) return 0;
        if (sw_crc(p->data + 2, 8u + length) != read16(p->data + 10 + length)) {
            discard(p, 1); ++p->rejected; continue;
        }
        frame->type = p->data[3]; frame->sequence = read32(p->data + 4);
        frame->length = length;
        memcpy(frame->payload, p->data + 10, length);
        discard(p, size);
        return 1;
    }
    return 0;
}
int sw_wire_self_test(void) {
    static const uint8_t text[] = "123456789";
    if (sw_crc(text, 9) != 0x29b1) return 0;
    uint8_t payload[9], good[SW_MAX_FRAME], bad[SW_MAX_FRAME];
    sw_write_token(payload, UINT64_C(0x123456789abcdef0)); payload[8] = SW_OK;
    size_t size = sw_encode(SW_ACK, 0x01020304, payload, 9, good, sizeof(good));
    if (size != 21) return 0;
    sw_parser_t parser = {0}; sw_frame_t frame = {0};
    /* Repeated AA, noise, corrupted frame, followed immediately by valid ACK. */
    sw_parser_feed(&parser, 0x42, &frame); sw_parser_feed(&parser, 0xaa, &frame);
    memcpy(bad, good, size); bad[size - 1] ^= 1;
    for (size_t i = 0; i < size; ++i) if (sw_parser_feed(&parser, bad[i], &frame)) return 0;
    for (size_t i = 0; i < size; ++i) {
        int ready = sw_parser_feed(&parser, good[i], &frame);
        if (ready != (i == size - 1)) return 0;
    }
    if (frame.type != SW_ACK || frame.sequence != 0x01020304 || frame.length != 9 ||
        sw_read_token(frame.payload) != UINT64_C(0x123456789abcdef0) || !parser.rejected) return 0;
    /* Sticky packets and ring producer boundaries are byte independent. */
    for (unsigned n = 0; n < 100; ++n) {
        unsigned ready = 0;
        for (size_t i = 0; i < size; ++i) ready += sw_parser_feed(&parser, good[i], &frame);
        if (ready != 1) return 0;
    }
    uint8_t huge[] = {0xaa,0x55,1,9,0,0,0,0,0xff,0xff};
    for (size_t i = 0; i < sizeof(huge); ++i) sw_parser_feed(&parser, huge[i], &frame);
    unsigned ready = 0;
    for (size_t i = 0; i < size; ++i) ready += sw_parser_feed(&parser, good[i], &frame);
    return ready == 1;
}
