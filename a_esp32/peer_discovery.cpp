#include "peer_discovery.h"
#include <ESPmDNS.h>

static constexpr uint32_t HOST_QUERY_MS = 1500;
static constexpr int32_t CONNECT_MS = 1500;
static constexpr uint32_t CACHE_TTL_MS = 300000;

void PeerDiscovery::reset() {
    if (mdns_ready_) MDNS.end();
    mdns_ready_ = false;
    cached_ = IPAddress(); cached_at_ = 0;
    local_ip_ = IPAddress(); mask_ = IPAddress();
}
bool PeerDiscovery::updateNetwork() {
    const IPAddress current = WiFi.localIP(), mask = WiFi.subnetMask();
    if (current == local_ip_ && mask == mask_) return false;
    reset(); local_ip_ = current; mask_ = mask;
    return true;
}
bool PeerDiscovery::localAddress(const IPAddress &ip) const {
    if (ip.type() != IPv4 || static_cast<uint32_t>(ip) == 0 ||
        ip[0] == 0 || ip[0] == 127 || ip[0] >= 224 || ip == local_ip_) return false;
    const uint32_t bits = static_cast<uint32_t>(ip);
    const uint32_t local = static_cast<uint32_t>(local_ip_);
    const uint32_t mask = static_cast<uint32_t>(mask_);
    if (!local || !mask || (bits & mask) != (local & mask)) return false;
    const uint32_t host_bits = bits & ~mask;
    return host_bits != 0 && host_bits != ~mask;
}
bool PeerDiscovery::tryAddress(WiFiClient &client, const IPAddress &ip,
                               const char *method, const char *&source) {
    if (!localAddress(ip)) return false;
    Serial.printf("Connecting to %s via %s ...\n", ip.toString().c_str(), method);
    if (!client.connect(ip, port_, CONNECT_MS)) {
        client.stop();
        if (cached_ == ip) { cached_ = IPAddress(); cached_at_ = 0; }
        Serial.printf("Connect failed via %s; stale cache cleared if matching\n", method);
        return false;
    }
    cached_ = ip; cached_at_ = millis(); source = method;
    return true;
}
bool PeerDiscovery::connect(WiFiClient &client, const char *&source) {
    source = "none";
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!mdns_ready_) {
        mdns_ready_ = MDNS.begin("esp32-a");
        if (mdns_ready_) Serial.println("mDNS initialized hostname=esp32-a");
        else { MDNS.end(); Serial.println("mDNS init failed; retry next connection attempt"); }
    }
    IPAddress resolved;
    if (mdns_ready_) {
        // queryHost expects the bare hostname, without the .local suffix.
        resolved = MDNS.queryHost(host_, HOST_QUERY_MS);
        if (tryAddress(client, resolved, "mDNS-host", source)) return true;
    }
    const bool valid_cache = localAddress(cached_) &&
                             (uint32_t)(millis() - cached_at_) < CACHE_TTL_MS;
    if (valid_cache && cached_ != resolved) {
        if (tryAddress(client, cached_, "cache", source)) return true;
    } else if (!valid_cache) {
        cached_ = IPAddress(); cached_at_ = 0;
    }
    // Discover the advertised service too, including a renamed hostname.
    // Arduino 3.3.11 bounds queryService at 3000ms and at most 20 results.
    if (mdns_ready_) {
        const int found = MDNS.queryService("esp32b", "tcp");
        unsigned attempts = 0;
        for (int i = 0; i < found && attempts < 3; ++i) {
            const String name = MDNS.hostname(i);
            if (MDNS.port(i) != port_ ||
                !(name == host_ || name == String(host_) + ".local" ||
                  name.startsWith(String(host_) + "-"))) continue;
            const IPAddress address = MDNS.address(i);
            if (address == resolved || !localAddress(address)) continue;
            ++attempts;
            if (tryAddress(client, address, "mDNS-service", source)) return true;
        }
    }
    if (fallback_ && fallback_[0]) {
        IPAddress fallback;
        if (!fallback.fromString(fallback_) || !localAddress(fallback)) {
            Serial.println("Configured fallback is outside current subnet or invalid; ignored");
        } else if (fallback != resolved && tryAddress(client, fallback, "configured-fallback", source)) {
            return true;
        }
    }
    Serial.println("B not found/reachable; discovery will retry");
    return false;
}
