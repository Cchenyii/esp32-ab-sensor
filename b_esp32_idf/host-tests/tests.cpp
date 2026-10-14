#include "protocol_self_test.h"
#include "protocol.h"
#include "session_gate.h"
#include "session_wire.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using SensorProtocol::Frame;
using UartLink::SessionGate;
static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
static Frame request() { Frame f{}; f.version = 1; f.type = SW_REQUEST; return f; }
static Frame confirm(uint64_t token) {
    auto f = request(); f.type = SW_CONFIRM; f.payloadLength = 8;
    sw_write_token(f.payload, token); return f;
}
static Frame joystick(uint64_t token, uint32_t seq) {
    auto f = confirm(token); f.type = SW_JOYSTICK; f.sequence = seq; f.payloadLength = 14;
    f.payload[8] = 8; f.payload[9] = 1; f.payload[10] = 8; f.payload[11] = 2;
    f.payload[12] = 0; f.payload[13] = 1; return f;
}
static uint64_t establish(SessionGate &gate, uint64_t now = 0) {
    auto o = gate.handle(request(), now);
    require(o.reply && o.replyType == SW_OFFER && o.token, "offer");
    require(gate.handle(confirm(o.token), now + 1).newSession, "confirm"); return o.token;
}
static void session_case(const std::string &name) {
    SessionGate gate(100);
    if (name == "session_existing_selftest") { require(UartLink::session_self_test(), "existing session suite"); return; }
    if (name == "session_request_retry") {
        const auto a = gate.handle(request(), 0), b = gate.handle(request(), 9999);
        require(a.token == b.token && a.token == 100 && !gate.activeToken(), "idempotent request"); return;
    }
    uint64_t token = establish(gate);
    if (name == "session_confirmation_retry") {
        require(gate.handle(joystick(token, 40), 2).accepted, "first sample");
        require(!gate.handle(confirm(token), 3).newSession, "retry must not reset");
        require(gate.handle(joystick(token, 40), 4).duplicate, "dedup retained");
    } else if (name == "session_stm32_reboot") {
        require(gate.handle(joystick(token, 1000), 2).accepted, "old session sample");
        uint64_t fresh = establish(gate, 10);
        require(fresh != token && gate.handle(joystick(fresh, 1), 12).accepted, "new session seq1");
    } else if (name == "session_b_reboot") {
        SessionGate rebooted(900, false);
        require(rebooted.handle(joystick(token, 1), 0).status == SW_NEED_SESSION, "reject old session after reboot");
        const auto fresh = establish(rebooted);
        require(rebooted.handle(joystick(fresh, 1), 2).accepted, "reboot recovery");
    } else if (name == "session_old_token") {
        uint64_t fresh = establish(gate, 10);
        require(gate.handle(confirm(token), 12).status == SW_NEED_SESSION, "old confirm");
        require(gate.handle(joystick(token, 2), 13).status == SW_NEED_SESSION, "old data");
        require(gate.activeToken() == fresh, "active token retained");
    } else if (name == "session_sequence_wrap") {
        require(gate.handle(joystick(token, 0xfffffffeU), 2).accepted, "pre-wrap");
        require(gate.handle(joystick(token, 1), 3).accepted, "post-wrap");
        require(gate.handle(joystick(token, 0xffffffffU), 4).duplicate, "stale pre-wrap");
    } else if (name == "session_offer_expiry") {
        auto pending = gate.handle(request(), 10);
        require(gate.handle(confirm(pending.token), 10010).status == SW_NEED_SESSION, "expired offer");
        require(gate.activeToken() == token, "expiry cannot replace active session");
    } else if (name == "session_invalid_joystick") {
        auto f = joystick(token, 1); f.payload[8] = 0x10; f.payload[9] = 0;
        require(gate.handle(f, 2).status == SW_BAD_LENGTH, "ADC bounds");
        f = joystick(token, 1); f.payload[12] = 5;
        require(gate.handle(f, 3).status == SW_BAD_LENGTH, "direction bounds");
        f = joystick(token, 1); f.payload[13] = 2;
        require(gate.handle(f, 4).status == SW_BAD_LENGTH, "switch bounds");
        require(gate.handle(joystick(token, 1), 5).accepted, "invalid data cannot advance sequence");
    } else if (name == "session_legacy_gate") {
        Frame f{}; f.type = SensorProtocol::JOYSTICK_DATA; f.payloadLength = 6;
        require(!gate.handle(f, 2).reply, "legacy disabled after session");
        SessionGate closed(5, false); require(!closed.handle(f, 0).reply, "legacy explicitly disabled");
    } else throw std::runtime_error("unknown session case");
}
static void codec_case(const std::string &name) {
    uint8_t wire[SW_MAX_FRAME]{};
    if (name == "codec_encoder_bounds") {
        uint8_t p[65]{};
        require(!sw_encode(9, 0, p, 65, wire, sizeof(wire)), "oversize payload");
        require(!sw_encode(9, 0, nullptr, 1, wire, sizeof(wire)), "null payload");
        require(!sw_encode(9, 0, p, 9, wire, 20), "short destination");
        require(!sw_encode(9, 0, p, 9, nullptr, sizeof(wire)), "null destination");
        require(sw_encode(9, 0, p, 64, wire, sizeof(wire)) == sizeof(wire), "max payload");
    } else if (name == "codec_sensor_validation") {
        sw_sensor_t input{}; input.token = 123; input.valid_for_ms = 4000; input.flags = 15;
        input.temperature_centi_c = -1234; input.humidity_centi_percent = 5500; input.distance_cm = 20;
        size_t size = sw_encode_sensor(123, &input, wire, sizeof(wire));
        sw_parser_t parser{}; sw_frame_t frame{}; int count = 0;
        for (size_t i=0; i<size; ++i) count += sw_parser_feed(&parser, wire[i], &frame);
        sw_sensor_t output{};
        require(count == 1 && sw_decode_sensor(&frame, &output) && output.temperature_centi_c == -1234, "signed sensor round trip");
        frame.payload[26] = 0x80; require(!sw_decode_sensor(&frame, &output), "reserved flags");
        frame.payload[26] = 1; require(!sw_decode_sensor(&frame, &output), "incomplete valid prerequisites");
        frame.payload[26] = 0; require(!sw_decode_sensor(&frame, &output), "invalid snapshot must have zero lease");
    } else if (name == "codec_parser_bounds") {
        sw_parser_t parser{}; sw_frame_t frame{};
        const uint8_t malformed[] = {0xaa,0x55,1,9,0,0,0,0,0xff,0xff};
        for (auto byte: malformed) require(!sw_parser_feed(&parser,byte,&frame), "malformed accepted");
        size_t size = sw_encode(SW_REQUEST,0,nullptr,0,wire,sizeof(wire)); int count=0;
        for (size_t i=0;i<size;++i) count += sw_parser_feed(&parser,wire[i],&frame);
        require(count==1 && frame.type==SW_REQUEST && parser.used<=SW_MAX_FRAME, "length resync");
    } else throw std::runtime_error("unknown codec case");
}
static void random_case(const std::string &name) {
    std::mt19937 random(0x20261014U);
    if (name == "random_session_dedup") {
        SessionGate gate(123); auto token = establish(gate);
        uint32_t seq=0;
        for(uint32_t round=0;round<2000;++round) {
            seq+=1+random()%17;
            require(gate.handle(joystick(token,seq),round+2).accepted,"fresh sequence");
            require(gate.handle(joystick(token,seq),round+2).duplicate,"repeated sequence");
            uint32_t old=seq-1-random()%std::min<uint32_t>(seq,100);
            require(gate.handle(joystick(token,old),round+2).duplicate,"old sequence");
        }
        return;
    }
    for(uint32_t round=0;round<2000;++round) {
        uint8_t p[64]{}, a[76]{}, b[76]{}; uint16_t length=static_cast<uint16_t>(random()%65);
        for (auto &byte:p) byte=static_cast<uint8_t>(random()&0x7f);
        size_t size=sw_encode(SW_ACK,round+1,p,length,a,sizeof(a));
        if(name=="random_cross_codec") {
            size_t other=SensorProtocol::encodeFrame(SW_ACK,round+1,p,length,b,sizeof(b));
            require(size==other && !std::memcmp(a,b,size),"C/C++ wire mismatch"); continue;
        }
        std::vector<uint8_t> stream{0x00,0x7e,0xaa,0x00};
        if(name=="random_crc_recovery") {
            std::copy(a,a+size,b); b[size-1]^=1; stream.insert(stream.end(),b,b+size);
        } else require(name=="random_fragmentation","unknown random case");
        stream.insert(stream.end(),a,a+size);
        SensorProtocol::FrameParser parser; Frame parsed{}; unsigned ready=0, errors=0;
        for(size_t offset=0;offset<stream.size();) {
            size_t chunk=std::min<size_t>(1+random()%17,stream.size()-offset);
            require(parser.push(stream.data()+offset,chunk)!=SensorProtocol::ParseResult::BUFFER_OVERFLOW,"unexpected overflow");
            offset+=chunk;
            for (;;) {
                auto outcome=parser.next(parsed); if(outcome==SensorProtocol::ParseResult::NONE) break;
                if(outcome!=SensorProtocol::ParseResult::FRAME_READY) { ++errors; continue; }
                ++ready; require(parsed.sequence==round+1 && parsed.payloadLength==length && !std::memcmp(parsed.payload,p,length),"fragmented data mismatch");
            }
        }
        require(ready==1 && !parser.bufferedBytes(),"random recovery frame count");
        if(name=="random_crc_recovery") require(errors>0,"corrupt frame not rejected");
    }
}
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    const std::string name=argv[1];
    try {
        sensor_protocol_self_test_report_t report{}; sensor_protocol_run_self_tests(&report);
        for(size_t i=0;i<report.count;++i) if(name==report.results[i].name) {
            require(report.results[i].passed,"existing protocol test"); std::printf("PASS %s\n",argv[1]); return 0;
        }
        if(name.rfind("session_",0)==0) session_case(name);
        else if(name.rfind("codec_",0)==0) codec_case(name);
        else random_case(name);
        std::printf("PASS %s\n",argv[1]); return 0;
    } catch(const std::exception &error) { std::fprintf(stderr,"FAIL %s: %s\n",argv[1],error.what()); return 1; }
}
