#pragma once
#include <WiFi.h>

/* Owned only by TCPTask. Cache addresses only after a successful connection. */
class PeerDiscovery {
public:
    PeerDiscovery(const char *host, uint16_t port, const char *fallback) :
        host_(host), port_(port), fallback_(fallback) {}
    void reset();
    bool updateNetwork();
    bool connect(WiFiClient &client, const char *&source);
private:
    bool localAddress(const IPAddress &ip) const;
    bool tryAddress(WiFiClient &client, const IPAddress &ip, const char *method,
                    const char *&source);
    const char *host_;
    uint16_t port_;
    const char *fallback_;
    bool mdns_ready_ = false;
    IPAddress local_ip_, mask_, cached_;
    uint32_t cached_at_ = 0;
};
