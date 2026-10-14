#include "cloud_settings.h"
#if __has_include("thingsboard_credentials.h")
#include "thingsboard_credentials.h"
#else
#include "thingsboard_credentials.example.h"
#endif
#ifndef TB_ALLOW_PLAINTEXT
#define TB_ALLOW_PLAINTEXT 0
#endif
/* Separate translation unit keeps the complete MQTT path in unconfigured builds. */
const char *cloud_broker_uri(void) { return TB_MQTT_URI; }
const char *cloud_access_token(void) { return TB_ACCESS_TOKEN; }
bool cloud_allow_plaintext(void) { return TB_ALLOW_PLAINTEXT != 0; }
