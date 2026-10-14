#pragma once
#include "protocol.h"
#include "session_wire.h"

namespace UartLink {
struct Outcome {
    bool reply = false;
    bool accepted = false;
    bool duplicate = false;
    bool newSession = false;
    uint8_t replyType = SW_ACK;
    uint8_t status = SW_OK;
    uint64_t token = 0;
    uint32_t sequence = 0;
    SensorProtocol::JoystickPayload joystick{};
};
class SessionGate {
public:
    explicit SessionGate(uint64_t seed, bool allowLegacy = true) :
        nextToken_(seed ? seed : 1), allowLegacy_(allowLegacy) {}
    Outcome handle(const SensorProtocol::Frame &frame, uint64_t nowMs);
    uint64_t activeToken() const { return active_; }
private:
    uint64_t nextToken_, active_ = 0, pending_ = 0, pendingAt_ = 0;
    uint32_t last_ = 0, legacyLast_ = 0;
    bool hasLast_ = false, hasLegacyLast_ = false, allowLegacy_;
};
bool session_self_test();
}
