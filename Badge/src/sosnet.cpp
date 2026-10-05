#include "sosnet.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static esp_mqtt_client_handle_t s_client = nullptr;
static QueueHandle_t s_rx = nullptr;
static volatile bool s_connected = false;
static char s_status[48] = "off";
static char s_uri[112], s_cid[40], s_user[40], s_pass[72], s_online[48];
static char s_subs[SOSNET_MAX_SUBS][64];
static int s_nsubs = 0;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      s_connected = true;
      snprintf(s_status, sizeof(s_status), "connected");
      for (int i = 0; i < s_nsubs; i++) esp_mqtt_client_subscribe(s_client, s_subs[i], 1);
      esp_mqtt_client_publish(s_client, s_online, "1", 1, 1, 1);
      printf("sos    : MQTT connected (%s)\n", s_uri);
      break;
    case MQTT_EVENT_DISCONNECTED:
      s_connected = false;
      snprintf(s_status, sizeof(s_status), "reconnecting");
      printf("sos    : MQTT disconnected - retrying\n");
      break;
    case MQTT_EVENT_ERROR:
      if (e && e->error_handle) {
        if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
          snprintf(s_status, sizeof(s_status), "refused (login?)");
          printf("sos    : MQTT refused, code %d - check user/password\n", e->error_handle->connect_return_code);
        } else if (e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
          snprintf(s_status, sizeof(s_status), "cannot reach broker");
          printf("sos    : MQTT transport error (tls 0x%x, sock errno %d)\n",
                 (unsigned)e->error_handle->esp_tls_last_esp_err, e->error_handle->esp_transport_sock_errno);
        }
      }
      break;
    case MQTT_EVENT_DATA:
      // Our messages are tiny and arrive in one piece; anything fragmented is not ours.
      if (e->topic_len > 0 && e->topic_len < (int)sizeof(SosMsg::topic) &&
          e->data_len < (int)sizeof(SosMsg::payload) && e->total_data_len == e->data_len) {
        SosMsg m;
        memcpy(m.topic, e->topic, e->topic_len);   m.topic[e->topic_len] = 0;
        memcpy(m.payload, e->data, e->data_len);   m.payload[e->data_len] = 0;
        if (s_rx) xQueueSend(s_rx, &m, 0);          // never block the MQTT task
      }
      break;
    default:
      break;
  }
}

bool sosnet_start(const char *host, int port, bool tls, const char *clientId, const char *user,
                  const char *pass, const char *onlineTopic, const char *const *subs, int nsubs) {
  sosnet_stop();
  if (!s_rx) s_rx = xQueueCreate(24, sizeof(SosMsg));   // ~6.5 kB; a reconnect replays retained polls + votes
  snprintf(s_uri, sizeof(s_uri), "%s://%s:%d", tls ? "mqtts" : "mqtt", host, port);
  snprintf(s_cid, sizeof(s_cid), "%s", clientId);
  snprintf(s_user, sizeof(s_user), "%s", user);
  snprintf(s_pass, sizeof(s_pass), "%s", pass);
  snprintf(s_online, sizeof(s_online), "%s", onlineTopic);
  s_nsubs = nsubs > SOSNET_MAX_SUBS ? SOSNET_MAX_SUBS : nsubs;
  for (int i = 0; i < s_nsubs; i++) snprintf(s_subs[i], sizeof(s_subs[i]), "%s", subs[i]);

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = s_uri;
  if (tls) cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.credentials.client_id = s_cid;
  cfg.credentials.username = s_user;
  cfg.credentials.authentication.password = s_pass;
  cfg.session.keepalive = 30;
  cfg.session.disable_clean_session = true;     // slaps sent while we are away wait for us (QoS 1)
  cfg.session.last_will.topic = s_online;
  cfg.session.last_will.msg = "0";
  cfg.session.last_will.msg_len = 1;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.network.reconnect_timeout_ms = 10000;
  cfg.task.stack_size = 8192;                   // TLS handshakes need more than the default
  // v3.6: priority 1, the same as Arduino's loop(). The default (5) is above loop(), and this
  // core's esp-mqtt task is not pinned to a core - so a (re)connect's TLS handshake could land on
  // the UI core and freeze the carousel for a second or more. At 1 it shares instead of pre-empting,
  // and runs on core 0 whenever that core is idle.
  cfg.task.priority = 1;

  s_client = esp_mqtt_client_init(&cfg);
  if (!s_client) { snprintf(s_status, sizeof(s_status), "init failed"); return false; }
  esp_mqtt_client_register_event(s_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, on_event, nullptr);
  snprintf(s_status, sizeof(s_status), "connecting");
  return esp_mqtt_client_start(s_client) == ESP_OK;
}

void sosnet_stop() {
  if (!s_client) return;
  if (s_connected) esp_mqtt_client_publish(s_client, s_online, "0", 1, 1, 1);   // clean goodbye
  esp_mqtt_client_stop(s_client);
  esp_mqtt_client_destroy(s_client);
  s_client = nullptr;
  s_connected = false;
  snprintf(s_status, sizeof(s_status), "off");
}

bool sosnet_connected() { return s_connected; }

bool sosnet_publish(const char *topic, const char *payload, bool retain) {
  if (!s_client) return false;
  // enqueue = kept in the outbox and (re)sent by the client task, also across a short drop-out
  return esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, retain ? 1 : 0, true) >= 0;
}

bool sosnet_next(SosMsg *out) { return s_rx && xQueueReceive(s_rx, out, 0) == pdTRUE; }
int  sosnet_pending() { return s_rx ? (int)uxQueueMessagesWaiting(s_rx) : 0; }
const char *sosnet_status() { return s_status; }
