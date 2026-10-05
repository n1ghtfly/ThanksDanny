// sosnet - the badges' MQTT link (ESP-IDF esp-mqtt, built into the ESP32 core: no library).
// Kept in its own .cpp so Arduino's auto-prototyping never sees the esp-mqtt types.
#pragma once
#include <stdint.h>

struct SosMsg {
  char topic[64];
  char payload[208];        // v3.9: a poll ("<id>|question|answer|...") is up to ~170 bytes
};
#define SOSNET_MAX_SUBS 8

// Connect (or reconnect with new settings). Subscriptions are re-made on every (re)connect;
// `onlineTopic` gets a retained "1" when connected, and a retained "0" as the last will.
// Strings are copied. tls = mqtts:// checked against the built-in public CA bundle.
bool sosnet_start(const char *host, int port, bool tls, const char *clientId, const char *user,
                  const char *pass, const char *onlineTopic, const char *const *subs, int nsubs);
void sosnet_stop();
bool sosnet_connected();
// Queued in the client's outbox if not connected right now; sent when the link is back (QoS 1).
bool sosnet_publish(const char *topic, const char *payload, bool retain);
bool sosnet_next(SosMsg *out);        // pop one received message; false if none
int  sosnet_pending();                // how many received messages are waiting
const char *sosnet_status();          // short human-readable state for screens and logs
