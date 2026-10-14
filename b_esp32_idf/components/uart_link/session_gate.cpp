#include "session_gate.h"
#include <cstring>

namespace UartLink {
static bool newer(uint32_t value, uint32_t previous) {
    const uint32_t delta = value - previous;
    return delta != 0 && delta < UINT32_C(0x80000000);
}
static bool valid(const SensorProtocol::JoystickPayload &j) {
    return j.x <= 4095 && j.y <= 4095 && j.direction <= 4 && j.sw <= 1;
}
Outcome SessionGate::handle(const SensorProtocol::Frame &f, uint64_t now) {
    Outcome o; o.sequence = f.sequence;
    if (f.type == SW_REQUEST) {
        if (f.sequence || f.payloadLength) return o;
        if (!pending_ || now - pendingAt_ >= 10000) {
            pending_ = nextToken_++;
            if (!nextToken_) ++nextToken_;
            pendingAt_ = now;
        }
        o.reply = true; o.replyType = SW_OFFER; o.token = pending_;
        return o;
    }
    if (f.type == SW_CONFIRM) {
        if (f.sequence || f.payloadLength != 8) return o;
        o.reply = true; o.token = sw_read_token(f.payload);
        if (o.token && o.token == active_) return o; // retry never clears dedup
        if (o.token && o.token == pending_ && now - pendingAt_ < 10000) {
            active_ = pending_; pending_ = 0; hasLast_ = false;
            o.newSession = true; return o;
        }
        o.status = SW_NEED_SESSION; return o;
    }
    if (f.type == SW_JOYSTICK) {
        if (f.payloadLength < 8) return o; // token cannot be echoed
        o.reply = true; o.token = sw_read_token(f.payload);
        if (!active_ || o.token != active_) { o.status = SW_NEED_SESSION; return o; }
        if (f.payloadLength != 14) { o.status = SW_BAD_LENGTH; return o; }
        const auto *p = f.payload + 8;
        o.joystick = {static_cast<uint16_t>((p[0] << 8) | p[1]),
                      static_cast<uint16_t>((p[2] << 8) | p[3]), p[4], p[5]};
        if (!valid(o.joystick)) { o.status = SW_BAD_LENGTH; return o; }
        o.duplicate = hasLast_ && !newer(f.sequence, last_);
        if (!o.duplicate) { last_ = f.sequence; hasLast_ = true; o.accepted = true; }
        return o;
    }
    if (f.type == SensorProtocol::JOYSTICK_DATA && allowLegacy_ && !active_) {
        o.reply = true; o.replyType = SensorProtocol::ACK;
        if (!SensorProtocol::decodeJoystickPayload(f, o.joystick) || !valid(o.joystick)) {
            o.status = SW_BAD_LENGTH; return o;
        }
        o.duplicate = hasLegacyLast_ && !newer(f.sequence, legacyLast_);
        if (!o.duplicate) { legacyLast_ = f.sequence; hasLegacyLast_ = true; o.accepted = true; }
    }
    return o;
}
bool session_self_test() {
    if (!sw_wire_self_test()) return false;
    SessionGate gate(100);
    SensorProtocol::Frame f{};
    f.type = SW_REQUEST;
    auto o = gate.handle(f, 0);
    if (!o.reply || o.token != 100 || gate.handle(f, 1).token != 100) return false;
    f.type = SW_CONFIRM; f.payloadLength = 8; sw_write_token(f.payload, 100);
    if (!gate.handle(f, 2).newSession || gate.handle(f, 3).newSession) return false;
    f.type = SW_JOYSTICK; f.sequence = 40; f.payloadLength = 14;
    f.payload[8] = 8; f.payload[9] = 0; f.payload[10] = 8; f.payload[11] = 0;
    if (!gate.handle(f, 4).accepted || !gate.handle(f, 5).duplicate) return false;
    f.type = SW_CONFIRM; f.sequence = 0; f.payloadLength = 8;
    if (gate.handle(f, 6).newSession) return false;
    f.type = SW_JOYSTICK; f.sequence = 40; f.payloadLength = 14;
    if (!gate.handle(f, 7).duplicate) return false;
    // STM32 reboot: a new confirmed session accepts seq 1 immediately.
    f.type = SW_REQUEST; f.sequence = 0; f.payloadLength = 0;
    if (gate.handle(f, 8).token != 101) return false;
    f.type = SW_CONFIRM; f.payloadLength = 8; sw_write_token(f.payload, 101);
    if (!gate.handle(f, 9).newSession) return false;
    f.type = SW_JOYSTICK; f.sequence = 1; f.payloadLength = 14;
    if (!gate.handle(f, 10).accepted) return false;
    sw_write_token(f.payload, 100);
    if (gate.handle(f, 11).status != SW_NEED_SESSION) return false;
    f.type = SW_CONFIRM; f.sequence = 0; f.payloadLength = 8;
    if (gate.handle(f, 12).status != SW_NEED_SESSION) return false;
    f.type = SensorProtocol::JOYSTICK_DATA; f.payloadLength = 6;
    if (gate.handle(f, 13).accepted) return false;
    // B reboot invalidates tokens; wrap-around is accepted within a session.
    SessionGate rebooted(200, false);
    f.type = SW_JOYSTICK; f.payloadLength = 14;
    if (rebooted.handle(f, 0).status != SW_NEED_SESSION) return false;
    f.type = SW_REQUEST; f.payloadLength = 0; rebooted.handle(f, 1);
    f.type = SW_CONFIRM; f.payloadLength = 8; sw_write_token(f.payload, 200);
    if (!rebooted.handle(f, 2).newSession) return false;
    f.type = SW_JOYSTICK; f.payloadLength = 14; f.sequence = 0xfffffffe;
    if (!rebooted.handle(f, 3).accepted) return false;
    f.sequence = 1; if (!rebooted.handle(f, 4).accepted) return false;
    f.sequence = 0xffffffff; if (!rebooted.handle(f, 5).duplicate) return false;
    // Offer expiry; a delayed confirmation must not replace the active session.
    f.type = SW_REQUEST; f.sequence = 0; f.payloadLength = 0;
    auto expired = rebooted.handle(f, 6);
    f.type = SW_CONFIRM; f.payloadLength = 8; sw_write_token(f.payload, expired.token);
    if (rebooted.handle(f, 10006).status != SW_NEED_SESSION) return false;
    // Cross-check the C codec used by STM32 against the original C++ codec.
    uint8_t a[76], b[76], p[9] = {0}; sw_write_token(p, 200);
    size_t na = sw_encode(SW_ACK, 123, p, 9, a, sizeof(a));
    size_t nb = SensorProtocol::encodeFrame(SW_ACK, 123, p, 9, b, sizeof(b));
    return na == nb && !std::memcmp(a, b, na);
}
}
