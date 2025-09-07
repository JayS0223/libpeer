#ifndef PEER_SIGNALING_H_
#define PEER_SIGNALING_H_

#include <core_http_client.h>
#include <stdbool.h>
#include "peer_connection.h"
#ifdef __cplusplus
extern "C" {
#endif

#ifndef DISABLE_PEER_SIGNALING

typedef struct ServiceConfiguration {
  const char* mqtt_url;
  int mqtt_port;
  const char* client_id;
  const char* http_url;
  int http_port;
  const char* username;
  const char* password;
  const char* bearer_token;
  PeerConnection* pc;
  const char* hostname;
  const char* path;
  const char* auth_token;
} ServiceConfiguration;

#define SERVICE_CONFIG_DEFAULT()    \
  {                                 \
      .mqtt_url = "broker.emqx.io", \
      .mqtt_port = 8883,            \
      .client_id = "peer",          \
      .http_url = "",               \
      .http_port = 443,             \
      .username = "",               \
      .password = "",               \
      .bearer_token = "",           \
      .pc = NULL}

void peer_signaling_set_config(ServiceConfiguration* config);

int peer_signaling_whip_connect();

int peer_signaling_whep_connect();

bool get_publish_check();

bool get_subscribe_check();

void set_publish_check(bool value);

void set_subscribe_check(bool value);

HTTPResponse_t peer_signaling_http_request(const TransportInterface_t* transport_interface,
                                           const char* method,
                                           size_t method_len,
                                           const char* host,
                                           size_t host_len,
                                           const char* path,
                                           size_t path_len,
                                           const char* auth,
                                           size_t auth_len,
                                           const char* body,
                                           size_t body_len);
int delete_publish_peer_from_meeting();
int delete_subscribe_peer_from_meeting();

#ifdef __cplusplus
}
#endif

#endif  // DISABLE_PEER_SIGNALING

#endif  // PEER_SIGNALING_H_
