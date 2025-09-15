#ifndef DISABLE_PEER_SIGNALING
#include "peer_signaling.h"
#include <assert.h>
#include <cJSON.h>
#include <core_http_client.h>
#include <core_mqtt.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "base64.h"
#include "config.h"
#include "ports.h"
#include "ssl_transport.h"
#include "utils.h"

#define KEEP_ALIVE_TIMEOUT_SECONDS 60
#define CONNACK_RECV_TIMEOUT_MS 1000

#ifndef BUF_SIZE
#define BUF_SIZE 4096
#endif

#define TOPIC_SIZE 128

#define HOST_LEN 64
#define CRED_LEN 128
#define BEARER_TOKEN_LEN 1024

#define RPC_VERSION "2.0"

#define RPC_METHOD_STATE "state"
#define RPC_METHOD_OFFER "offer"
#define RPC_METHOD_ANSWER "answer"
#define RPC_METHOD_CLOSE "close"

#define RPC_ERROR_PARSE_ERROR "{\"code\":-32700,\"message\":\"Parse error\"}"
#define RPC_ERROR_INVALID_REQUEST "{\"code\":-32600,\"message\":\"Invalid Request\"}"
#define RPC_ERROR_METHOD_NOT_FOUND "{\"code\":-32601,\"message\":\"Method not found\"}"
#define RPC_ERROR_INVALID_PARAMS "{\"code\":-32602,\"message\":\"Invalid params\"}"
#define RPC_ERROR_INTERNAL_ERROR "{\"code\":-32603,\"message\":\"Internal error\"}"

typedef struct PeerSignaling {
  MQTTContext_t mqtt_ctx;
  MQTTFixedBuffer_t mqtt_fixed_buf;

  TransportInterface_t transport;
  NetworkContext_t net_ctx;

  uint8_t mqtt_buf[BUF_SIZE];
  uint8_t http_buf[BUF_SIZE];

  char subtopic[TOPIC_SIZE];
  char pubtopic[TOPIC_SIZE];

  uint16_t packet_id;
  int id;

  int mqtt_port;
  int http_port;
  char mqtt_host[HOST_LEN];
  char http_host[HOST_LEN];
  char http_path[HOST_LEN];
  char username[CRED_LEN];
  char password[CRED_LEN];
  char bearer_token[BEARER_TOKEN_LEN];
  char client_id[CRED_LEN];
  PeerConnection* pc;

} PeerSignaling;

static PeerSignaling g_ps;
char* g_hostname = NULL;
char* g_token = NULL;
char* g_path = NULL;
bool publish_check = false;
bool subscribe_check = false;
bool get_publish_check() {
  return publish_check;
}
bool get_subscribe_check() {
  return subscribe_check;
}

void set_publish_check(bool value) {
  publish_check = value;
}

void set_subscribe_check(bool value) {
  subscribe_check = value;
}
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
                                           size_t body_len) {
  HTTPStatus_t status = HTTPSuccess;
  HTTPRequestInfo_t request_info = {0};
  HTTPResponse_t response = {0};
  HTTPRequestHeaders_t request_headers = {0};

  request_info.pMethod = method;
  request_info.methodLen = method_len;
  request_info.pHost = host;
  request_info.hostLen = host_len;
  request_info.pPath = path;
  request_info.pathLen = path_len;
  request_info.reqFlags = HTTP_REQUEST_KEEP_ALIVE_FLAG;

  request_headers.pBuffer = g_ps.http_buf;
  request_headers.bufferLen = sizeof(g_ps.http_buf);

  status = HTTPClient_InitializeRequestHeaders(&request_headers, &request_info);

  if (status == HTTPSuccess) {
    HTTPClient_AddHeader(&request_headers,
                         "Content-Type", strlen("Content-Type"), "application/sdp", strlen("application/sdp"));

    if (auth_len > 0) {
      HTTPClient_AddHeader(&request_headers,
                           "Authorization", strlen("Authorization"), auth, auth_len);
    }

    response.pBuffer = g_ps.http_buf;
    response.bufferLen = sizeof(g_ps.http_buf);

    status = HTTPClient_Send(transport_interface,
                             &request_headers, (uint8_t*)body, body ? body_len : 0, &response, 0);

  } else {
    LOGE("Failed to initialize HTTP request headers: Error=%s.", HTTPClient_strerror(status));
  }

  return response;
}

char auth_header[1024];

char resource_url[256] = {0};
char resource_url_publish[256] = {0};
char resource_url_subscribe[256] = {0};
char* g_body = NULL;

static int peer_signaling_http_post(const char* hostname, const char* path, int port, const char* auth, const char* body) {
  int ret = 0;
  g_body = (char*)body;
  TransportInterface_t trans_if = {0};
  NetworkContext_t net_ctx;
  HTTPResponse_t res;
  trans_if.recv = ssl_transport_recv;
  trans_if.send = ssl_transport_send;
  trans_if.pNetworkContext = &net_ctx;

  if (port <= 0) {
    LOGE("Invalid port number: %d", port);
    return -1;
  }

  if (body == NULL || strlen(body) == 0) {
    LOGE("Body is NULL or empty");
    return -1;
  }
  snprintf(auth_header, sizeof(auth_header), "%s", g_token);
  {
    int attempt;
    memset(&res, 0, sizeof(res));
    for (attempt = 0; attempt < 3; ++attempt) {
      ret = ssl_transport_connect(&net_ctx, hostname, port, NULL);
      if (ret < 0) {
        LOGE("Failed to connect to %s:%d", hostname, port);
        return ret;
      }

      res = peer_signaling_http_request(
          &trans_if,
          "POST", strlen("POST"),
          g_hostname, strlen(g_hostname),
          g_path, strlen(g_path),
          auth_header, strlen(auth_header),
          body, strlen(body));

      ssl_transport_disconnect(&net_ctx);

      if (res.pHeaders == NULL) {
        LOGW("POST attempt %d: response headers are NULL, retrying...", attempt + 1);
        continue;
      }
      break;
    }

    if (res.pHeaders == NULL) {
      LOGE("Response headers are NULL after 3 attempts");
      return -1;
    }
  }

 if (res.pHeaders) {
    char* location_line = strstr((char*)res.pHeaders, "Location:");
    if (location_line) {
        location_line += 9;
        while (*location_line == ' ')
            location_line++;
        char* end = strstr(location_line, "\r\n");
        if (!end)
            end = strstr(location_line, "\n");
        if (end && (end - location_line) < sizeof(resource_url)) {
            strncpy(resource_url, location_line, end - location_line);
            resource_url[end - location_line] = '\0';

            if (strncmp(resource_url, "https://", 8) == 0) {
                char* path_start = strchr(resource_url + 8, '/');
                if (path_start) {
                    memmove(resource_url, path_start, strlen(path_start) + 1);
                } else {
                    LOGE("Invalid Location URL: No path found.");
                    return -1;
                }
            }

        if (strstr(resource_url, "whip")) {
    if (strlen(resource_url) > 0) {
        strncpy(resource_url_publish, resource_url, sizeof(resource_url_publish) - 1);
        resource_url_publish[sizeof(resource_url_publish) - 1] = '\0';
    } else {
        LOGE("Empty publish resource URL.");
        set_publish_check(false);
    }
} else {
    if (strlen(resource_url) > 0) {
        strncpy(resource_url_subscribe, resource_url, sizeof(resource_url_subscribe) - 1);
        resource_url_subscribe[sizeof(resource_url_subscribe) - 1] = '\0';
    } else {
        LOGE("Empty subscribe resource URL.");
        set_subscribe_check(false);
    }
}
        } else {
            LOGE("Failed to parse Location header.");
            return -1;
        }
    } else {
        LOGE("Location header not found.");
        return -1;
    }
}
  if (res.pHeaders == NULL) {
    LOGE("Response headers are NULL");
    return -1;
  }

  if (res.pBody == NULL) {
    LOGE("Response body is NULL");
    return -1;
  }

  if (res.statusCode == 201) {
    peer_connection_set_remote_description(g_ps.pc, (const char*)res.pBody);
  } else {
    LOGE("Body:%s", res.pBody);
    return -1;
  }
  return 0;
}

int delete_publish_peer_from_meeting() {
  TransportInterface_t trans_if;
  HTTPResponse_t res;
  NetworkContext_t net_ctx;
  int ret = 0;
  // Initialize transport interface
  //  if (transport_init(&trans_if, "dev-api.videosdk.live", 443,auth_header ) < 0) {
  //    LOGE("Failed to initialize transport");
  //    return -1;
  //  }
  ret = ssl_transport_connect(&net_ctx, "dev-api.videosdk.live", 443, NULL);

  if (strlen(resource_url) == 0) {
    LOGE("Resource URL is empty");
    return -1;
  }

  trans_if.recv = ssl_transport_recv;
  trans_if.send = ssl_transport_send;
  trans_if.pNetworkContext = &net_ctx;
  strncpy(resource_url, resource_url_publish, sizeof(resource_url) - 1);
  resource_url[sizeof(resource_url) - 1] = '\0';
  res = peer_signaling_http_request(&trans_if, "DELETE", strlen("DELETE"), "dev-api.videosdk.live", strlen("dev-api.videosdk.live"), resource_url,
                                    strlen(resource_url), auth_header, strlen(auth_header), g_body, strlen(g_body));
  if (res.pHeaders == NULL || res.pBody == NULL) {
    LOGE("DELETE response invalid");
    ssl_transport_disconnect(&net_ctx);
    return -1;
  }

  if (res.statusCode == 200) {
    LOGI("Peer removed from meeting successfully");
  } else {
    LOGE("Response Body: %s", res.pBody);
    LOGE("Failed to remove peer from meeting");
  }

  ssl_transport_disconnect(&net_ctx);
  return 0;
}

int delete_subscribe_peer_from_meeting() {
  TransportInterface_t trans_if;
  HTTPResponse_t res;
  NetworkContext_t net_ctx;
  int ret = 0;
  ret = ssl_transport_connect(&net_ctx, "dev-api.videosdk.live", 443, NULL);

  if (strlen(resource_url) == 0) {
    LOGE("Resource URL is empty");
    return -1;
  }

  trans_if.recv = ssl_transport_recv;
  trans_if.send = ssl_transport_send;
  trans_if.pNetworkContext = &net_ctx;
  strncpy(resource_url, resource_url_subscribe, sizeof(resource_url) - 1);
  resource_url[sizeof(resource_url) - 1] = '\0';

  res = peer_signaling_http_request(&trans_if, "DELETE", strlen("DELETE"), "dev-api.videosdk.live", strlen("dev-api.videosdk.live"), resource_url,
                                    strlen(resource_url), auth_header, strlen(auth_header), g_body, strlen(g_body));
  if (res.pHeaders == NULL || res.pBody == NULL) {
    LOGE("DELETE response invalid");
    ssl_transport_disconnect(&net_ctx);
    return -1;
  }

  if (res.statusCode == 200) {
    LOGI("Peer removed from meeting successfully");
  } else {
    LOGE("Failed to remove peer from meeting");
  }

  ssl_transport_disconnect(&net_ctx);
  return 0;
}

static void peer_signaling_onicecandidate(char* description, void* userdata) {
  cJSON* res;
  char* payload;
  char cred_plaintext[2 * CRED_LEN + 1];
  char cred_base64[2 * CRED_LEN + 10];

  // enable authentication
  if (strlen(g_ps.username) > 0 && strlen(g_ps.password) > 0) {
    snprintf(cred_plaintext, sizeof(cred_plaintext), "%s:%s", g_ps.username, g_ps.password);
    snprintf(cred_base64, sizeof(cred_base64), "Basic ");
    base64_encode((unsigned char*)cred_plaintext, strlen(cred_plaintext),
                  cred_base64 + strlen(cred_base64), sizeof(cred_base64) - strlen(cred_base64));
    LOGD("Basic Auth: %s", cred_base64);
    peer_signaling_http_post(g_ps.http_host, g_ps.http_path, g_ps.http_port, cred_base64, description);
  } else {
    peer_signaling_http_post(g_ps.http_host, g_ps.http_path, g_ps.http_port, g_ps.bearer_token, description);
  }
}

int peer_signaling_whip_connect() {
  if (g_ps.pc == NULL) {
    LOGW("PeerConnection is NULL");
    return -1;
  } else if (g_ps.http_port <= 0) {
    LOGW("Invalid HTTP port number: %d", g_ps.http_port);
    return -1;
  }

  peer_connection_create_offer(g_ps.pc);
  return 0;
}

int peer_signaling_whep_connect() {
  if (g_ps.pc == NULL) {
    LOGE("PeerConnection is NULL");
    return -1;
  } else if (g_ps.http_port <= 0) {
    LOGE("Invalid HTTP port number: %d", g_ps.http_port);
    return -1;
  }
  peer_connection_create_offer(g_ps.pc);
  return 0;
}

void peer_signaling_set_config(ServiceConfiguration* service_config) {
  char* pos;

  g_hostname = service_config->hostname;
  g_token = service_config->auth_token;
  g_path = service_config->path;

  memset(&g_ps, 0, sizeof(g_ps));

  do {
    if (service_config->http_url == NULL || strlen(service_config->http_url) == 0) {
      break;
    }

    if ((pos = strstr(service_config->http_url, "/")) != NULL) {
      strncpy(g_ps.http_host, service_config->http_url, pos - service_config->http_url);
      strncpy(g_ps.http_path, pos, HOST_LEN);
    } else {
      strncpy(g_ps.http_host, service_config->http_url, HOST_LEN);
    }

    g_ps.http_port = service_config->http_port;
    if (strlen(service_config->bearer_token) > 0) {
      snprintf(g_ps.bearer_token, BEARER_TOKEN_LEN, "Bearer %s", service_config->bearer_token);
    }

  } while (0);

  do {
    if (service_config->mqtt_url == NULL || strlen(service_config->mqtt_url) == 0) {
      break;
    }

    strncpy(g_ps.mqtt_host, service_config->mqtt_url, HOST_LEN);
    g_ps.mqtt_port = service_config->mqtt_port;
    LOGD("MQTT Host: %s, Port: %d", g_ps.mqtt_host, g_ps.mqtt_port);
  } while (0);

  if (service_config->client_id != NULL && strlen(service_config->client_id) > 0) {
    strncpy(g_ps.client_id, service_config->client_id, CRED_LEN);
    snprintf(g_ps.subtopic, sizeof(g_ps.subtopic), "webrtc/%s/jsonrpc", service_config->client_id);
    snprintf(g_ps.pubtopic, sizeof(g_ps.pubtopic), "webrtc/%s/jsonrpc-reply", service_config->client_id);
  }

  if (service_config->username != NULL && strlen(service_config->username) > 0) {
    strncpy(g_ps.username, service_config->username, CRED_LEN);
  }

  if (service_config->password != NULL && strlen(service_config->password) > 0) {
    strncpy(g_ps.password, service_config->password, CRED_LEN);
  }

  g_ps.pc = service_config->pc;
  peer_connection_onicecandidate(g_ps.pc, peer_signaling_onicecandidate);
}
#endif  // DISABLE_PEER_SIGNALING
