#ifndef DISABLE_PEER_SIGNALING
#include <assert.h>
#include <cJSON.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <core_http_client.h>
#include <core_mqtt.h>

#include "base64.h"
#include "config.h"
#include "peer_signaling.h"
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
char *g_hostname = NULL;
char *g_token = NULL;
char *g_path = NULL;
static void peer_signaling_mqtt_publish(MQTTContext_t* mqtt_ctx, const char* message) {
  MQTTStatus_t status;
  MQTTPublishInfo_t pub_info;

  memset(&pub_info, 0, sizeof(pub_info));

  pub_info.qos = MQTTQoS0;
  pub_info.retain = false;
  pub_info.pTopicName = g_ps.pubtopic;
  pub_info.topicNameLength = strlen(g_ps.pubtopic);
  pub_info.pPayload = message;
  pub_info.payloadLength = strlen(message);

  status = MQTT_Publish(mqtt_ctx, &pub_info, MQTT_GetPacketId(mqtt_ctx));
  if (status != MQTTSuccess) {
    LOGE("MQTT_Publish failed: Status=%s.", MQTT_Status_strerror(status));
  } else {
    LOGD("MQTT_Publish succeeded.");
  }
}

static void peer_signaling_on_pub_event(const char* msg, size_t size) {
  cJSON *req, *res, *item, *result, *error;
  int id = -1;
  char* payload = NULL;
  PeerConnectionState state;

  req = res = item = result = error = NULL;
  state = peer_connection_get_state(g_ps.pc);
  do {
    req = cJSON_Parse(msg);
    if (!req) {
      error = cJSON_CreateRaw(RPC_ERROR_PARSE_ERROR);
      LOGW("Parse json failed");
      break;
    }

    item = cJSON_GetObjectItem(req, "id");
    if (!item && !cJSON_IsNumber(item)) {
      error = cJSON_CreateRaw(RPC_ERROR_INVALID_REQUEST);
      LOGW("Cannot find id");
      break;
    }

    id = item->valueint;

    item = cJSON_GetObjectItem(req, "method");
    if (!item && cJSON_IsString(item)) {
      error = cJSON_CreateRaw(RPC_ERROR_INVALID_REQUEST);
      LOGW("Cannot find method");
      break;
    }

    if (strcmp(item->valuestring, RPC_METHOD_OFFER) == 0) {
      switch (state) {
        case PEER_CONNECTION_NEW:
        case PEER_CONNECTION_DISCONNECTED:
        case PEER_CONNECTION_FAILED:
        case PEER_CONNECTION_CLOSED: {
          g_ps.id = id;
          peer_connection_create_offer(g_ps.pc);
        } break;
        default: {
          error = cJSON_CreateRaw(RPC_ERROR_INTERNAL_ERROR);
        } break;
      }
    } else if (strcmp(item->valuestring, RPC_METHOD_ANSWER) == 0) {
      item = cJSON_GetObjectItem(req, "params");
      if (!item && !cJSON_IsString(item)) {
        error = cJSON_CreateRaw(RPC_ERROR_INVALID_PARAMS);
        LOGW("Cannot find params");
        break;
      }

      // if (state == PEER_CONNECTION_NEW) {
      //   peer_connection_set_remote_description(g_ps.pc, item->valuestring);
      //   result = cJSON_CreateString("");
      // }

      switch (state) {
        case PEER_CONNECTION_NEW:
        case PEER_CONNECTION_DISCONNECTED:
        case PEER_CONNECTION_FAILED:
        case PEER_CONNECTION_CLOSED: {
          g_ps.id = id;
          peer_connection_set_remote_description(g_ps.pc, item->valuestring);
        } break;
        default: {
          error = cJSON_CreateRaw(RPC_ERROR_INTERNAL_ERROR);
        } break;
      }

    } else if (strcmp(item->valuestring, RPC_METHOD_STATE) == 0) {
      result = cJSON_CreateString(peer_connection_state_to_string(state));

    } else if (strcmp(item->valuestring, RPC_METHOD_CLOSE) == 0) {
      peer_connection_close(g_ps.pc);
      result = cJSON_CreateString("");

    } else {
      error = cJSON_CreateRaw(RPC_ERROR_METHOD_NOT_FOUND);
      LOGW("Unsupport method");
    }

  } while (0);

  if (result || error) {
    res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "jsonrpc", RPC_VERSION);
    cJSON_AddNumberToObject(res, "id", id);

    if (result) {
      cJSON_AddItemToObject(res, "result", result);
    } else if (error) {
      cJSON_AddItemToObject(res, "error", error);
    }

    payload = cJSON_PrintUnformatted(res);

    if (payload) {
      peer_signaling_mqtt_publish(&g_ps.mqtt_ctx, payload);
      free(payload);
    }
    cJSON_Delete(res);
  }

  if (req) {
    cJSON_Delete(req);
  }
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
 char *g_body = NULL;
 
static int peer_signaling_http_post(const char* hostname, const char* path, int port, const char* auth, const char* body) {
printf("Inside the http_post function");
  int ret = 0;
g_body = (char*)body;
  TransportInterface_t trans_if = {0};
  NetworkContext_t net_ctx;
  HTTPResponse_t res;
  LOGI("Sending offer %s", body);
printf("Http_post_function above the trans.if");
  trans_if.recv = ssl_transport_recv;
  trans_if.send = ssl_transport_send;
  trans_if.pNetworkContext = &net_ctx;

  if (port <= 0) {
    LOGE("Invalid port number: %d", port);
    return -1;
  }

  // res = peer_signaling_http_request(&trans_if, "POST", 4, hostname, strlen(hostname), path,
  //                                   strlen(path), auth, strlen(auth), body, strlen(body));



printf("HTTP post request: %s %s%s\n", "POST", hostname, path);

  if (body == NULL || strlen(body) == 0) {
    LOGE("Body is NULL or empty");
    return -1;
  }
snprintf(auth_header, sizeof(auth_header), "%s", g_token);
printf("Auth Header:%s\n", auth_header);
printf("Above the HTTP request");
  {
    int attempt;
    memset(&res, 0, sizeof(res));
    for (attempt = 0; attempt < 3; ++attempt) {
      printf("SSL transport_connect above it");
      ret = ssl_transport_connect(&net_ctx, hostname, port, NULL);
      printf("SSL transport_connect below  it");
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
          body, strlen(body)
      );

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

// res = peer_signaling_http_request(
//     &trans_if,
//     "POST", strlen("POST"),
//     "dev-whip.videosdk.live", strlen("dev-whip.videosdk.live"),
//     "/whep", strlen("/whep"),
//     auth_header, strlen(auth_header),
//     body, strlen(body));
// printf("HTTP POST response: status code: %d\n", res.statusCode);
//   if (res.pHeaders == NULL || res.pBody == NULL) {
//     LOGE("POST response invalid");
//     ssl_transport_disconnect(&net_ctx);
//     return -1;
//   }

 if (res.pHeaders) {
  char *location_line = strstr((char *)res.pHeaders, "Location:");
  if (location_line) {
    location_line += 9;
    while (*location_line == ' ') location_line++;
    char *end = strstr(location_line, "\r\n");
    if (!end) end = strstr(location_line, "\n");
    if (end && (end - location_line) < sizeof(resource_url)) {
      strncpy(resource_url, location_line, end - location_line);
      resource_url[end - location_line] = '\0';

      if (strncmp(resource_url, "https://", 8) == 0) {
        char *path_start = strchr(resource_url + 8, '/');
        if (path_start) {
          memmove(resource_url, path_start, strlen(path_start) + 1);
        } else {
          LOGE("Invalid Location URL: No path found.");
          ssl_transport_disconnect(&net_ctx);
          return -1;
        }
      }
      printf("Extracted Resource Path: %s\n", resource_url);
      if (strstr(resource_url, "whip")) {
        strncpy(resource_url_publish, resource_url, sizeof(resource_url_publish) - 1);
        resource_url_publish[sizeof(resource_url_publish) - 1] = '\0';
        printf("Stored in resource_url_publish: %s\n", resource_url_publish);
      } else {
        strncpy(resource_url_subscribe, resource_url, sizeof(resource_url_subscribe) - 1);
        resource_url_subscribe[sizeof(resource_url_subscribe) - 1] = '\0';
        printf("Stored in resource_url_subscribe: %s\n", resource_url_subscribe);
      }

    } else {
      printf("Failed to parse Location header.\n");
    }
  } else {
    printf("Location header not found.\n");
  }
}
  



// char auth_header[256];
// snprintf(auth_header, sizeof(auth_header), "%s", "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJhcGlrZXkiOiI1MzE0YzVkZC0wN2MzLTRjZTgtYThmYi03ZmY0ZDZiMDdhYTIiLCJwZXJtaXNzaW9ucyI6WyJhbGxvd19qb2luIl0sImlhdCI6MTc1MTI2MTQ4MCwiZXhwIjoxNzgyNzk3NDgwfQ.yMH2talasjiL7ftJt2hl9h6_L96G1YU_tjujycAEi9M");
// printf("Auth Header:%s\n", auth_header);

// printf("HTTP post request: %s %s%s\n", "POST", hostname, path);

//   res = peer_signaling_http_request(
//     &trans_if,
//     "POST", strlen("POST"),
//     "us2.api.videosdk.live", strlen("us2.api.videosdk.live"),
//     "/v2/whip?roomId=54dr-h8c5-hsif&participantId=whip-peer", strlen("/v2/whip?roomId=54dr-h8c5-hsif&participantId=whip-peer"),
//     auth_header, strlen(auth_header),
//   body, strlen(body)
// );       


  ///ssl_transport_disconnect(&net_ctx);

  if (res.pHeaders == NULL) {
    LOGE("Response headers are NULL");
    return -1;
  }

  if (res.pBody == NULL) {
    LOGE("Response body is NULL");
    return -1;
  }

  LOGI(
      "Received HTTP response from %s%s\n"
      "Response Headers: %s\nResponse Status: %u\nResponse Body: %s\n",
      hostname, path, res.pHeaders, res.statusCode, res.pBody);

  if (res.statusCode == 201) {
    peer_connection_set_remote_description(g_ps.pc, (const char*)res.pBody);
  }
  return 0;
}

static void peer_signaling_mqtt_event_cb(MQTTContext_t* mqtt_ctx,
                                         MQTTPacketInfo_t* packet_info,
                                         MQTTDeserializedInfo_t* deserialized_info) {
  switch (packet_info->type) {
    case MQTT_PACKET_TYPE_CONNACK:
      LOGI("MQTT_PACKET_TYPE_CONNACK");
      break;
    case MQTT_PACKET_TYPE_PUBLISH:
      LOGI("MQTT_PACKET_TYPE_PUBLISH");
      peer_signaling_on_pub_event(deserialized_info->pPublishInfo->pPayload,
                                  deserialized_info->pPublishInfo->payloadLength);
      break;
    case MQTT_PACKET_TYPE_SUBACK:
      LOGD("MQTT_PACKET_TYPE_SUBACK");
      break;
    default:
      break;
  }
}

int delete_peer_from_meeting() {
  printf("Deleting peer from meeting: %s\n", resource_url);
  printf("Removing the peer !!");
  TransportInterface_t trans_if;
  HTTPResponse_t res;
  NetworkContext_t net_ctx;
int ret = 0;
  //Initialize transport interface
  // if (transport_init(&trans_if, "dev-api.videosdk.live", 443,auth_header ) < 0) {
  //   LOGE("Failed to initialize transport");
  //   return -1;
  // }
  ret = ssl_transport_connect(&net_ctx, "dev-api.videosdk.live", 443, NULL);

printf("Auth Header:%s\n", auth_header);
  if (strlen(resource_url) == 0) {
    LOGE("Resource URL is empty");
    return -1;
  }



 trans_if.recv = ssl_transport_recv;
  trans_if.send = ssl_transport_send;
  trans_if.pNetworkContext = &net_ctx;
if (resource_url_publish[0] != '\0') {
  strncpy(resource_url, resource_url_publish, sizeof(resource_url) - 1);
  resource_url[sizeof(resource_url) - 1] = '\0';
} else {
  strncpy(resource_url, resource_url_subscribe, sizeof(resource_url) - 1);
  resource_url[sizeof(resource_url) - 1] = '\0';
}
printf("resource url : %s", resource_url);
  
  printf("HTTP DELETE request: %s %s\n", "DELETE", resource_url);
     res = peer_signaling_http_request(&trans_if, "DELETE", strlen("DELETE"), "dev-api.videosdk.live", strlen("dev-api.videosdk.live"), resource_url,
                                      strlen(resource_url), auth_header, strlen(auth_header), g_body, strlen(g_body));
  printf("HTTP DELETE response: status code: %d\n", res.statusCode);
  if (res.pHeaders == NULL || res.pBody == NULL) {
    LOGE("DELETE response invalid");
   // ssl_transport_disconnect(&net_ctx);
    return -1;
  }

  if (res.statusCode == 200) {
    LOGI("Peer removed from meeting successfully");
  } else {
    LOGE("Failed to remove peer from meeting");
  }

 // ssl_transport_disconnect(&net_ctx);
  return 0;
}

static int peer_signaling_mqtt_connect(const char* hostname, int port) {
  MQTTStatus_t status;
  MQTTConnectInfo_t conn_info;
  bool session_present;

  if (ssl_transport_connect(&g_ps.net_ctx, hostname, port, NULL) < 0) {
    LOGE("ssl transport connect failed");
    return -1;
  }

  g_ps.transport.recv = ssl_transport_recv;
  g_ps.transport.send = ssl_transport_send;
  g_ps.transport.pNetworkContext = &g_ps.net_ctx;
  g_ps.mqtt_fixed_buf.pBuffer = g_ps.mqtt_buf;
  g_ps.mqtt_fixed_buf.size = sizeof(g_ps.mqtt_buf);
  status = MQTT_Init(&g_ps.mqtt_ctx, &g_ps.transport,
                     ports_get_epoch_time, peer_signaling_mqtt_event_cb, &g_ps.mqtt_fixed_buf);

  memset(&conn_info, 0, sizeof(conn_info));

  conn_info.cleanSession = false;
  if (strlen(g_ps.username) > 0) {
    conn_info.pUserName = g_ps.username;
    conn_info.userNameLength = strlen(g_ps.username);
  }

  if (strlen(g_ps.password) > 0) {
    conn_info.pPassword = g_ps.password;
    conn_info.passwordLength = strlen(g_ps.password);
  }

  if (strlen(g_ps.client_id) > 0) {
    conn_info.pClientIdentifier = g_ps.client_id;
    conn_info.clientIdentifierLength = strlen(g_ps.client_id);
  }

  conn_info.keepAliveSeconds = KEEP_ALIVE_TIMEOUT_SECONDS;

  status = MQTT_Connect(&g_ps.mqtt_ctx,
                        &conn_info, NULL, CONNACK_RECV_TIMEOUT_MS, &session_present);

  if (status != MQTTSuccess) {
    LOGE("MQTT_Connect failed: Status=%s.", MQTT_Status_strerror(status));
    return -1;
  }

  LOGI("MQTT_Connect succeeded.");
  return 0;
}

static int peer_signaling_mqtt_subscribe(int subscribed) {
  MQTTStatus_t status = MQTTSuccess;
  MQTTSubscribeInfo_t sub_info;

  uint16_t packet_id = MQTT_GetPacketId(&g_ps.mqtt_ctx);

  memset(&sub_info, 0, sizeof(sub_info));
  sub_info.qos = MQTTQoS0;
  sub_info.pTopicFilter = g_ps.subtopic;
  sub_info.topicFilterLength = strlen(g_ps.subtopic);

  if (subscribed) {
    status = MQTT_Subscribe(&g_ps.mqtt_ctx, &sub_info, 1, packet_id);
  } else {
    status = MQTT_Unsubscribe(&g_ps.mqtt_ctx, &sub_info, 1, packet_id);
  }
  if (status != MQTTSuccess) {
    LOGE("MQTT_Subscribe failed: Status=%s.", MQTT_Status_strerror(status));
    return -1;
  }

  status = MQTT_ProcessLoop(&g_ps.mqtt_ctx);

  if (status != MQTTSuccess) {
    LOGE("MQTT_ProcessLoop failed: Status=%s.", MQTT_Status_strerror(status));
    return -1;
  }

  LOGD("MQTT Subscribe/Unsubscribe succeeded.");
  return 0;
}

static void peer_signaling_onicecandidate(char* description, void* userdata) {
  cJSON* res;
  char* payload;
  char cred_plaintext[2 * CRED_LEN + 1];
  char cred_base64[2 * CRED_LEN + 10];

  if (g_ps.id > 0) {
    res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "jsonrpc", RPC_VERSION);
    cJSON_AddNumberToObject(res, "id", g_ps.id);
    cJSON_AddStringToObject(res, "result", description);
    payload = cJSON_PrintUnformatted(res);
    if (payload) {
      peer_signaling_mqtt_publish(&g_ps.mqtt_ctx, payload);
      free(payload);
    }
    cJSON_Delete(res);
    g_ps.id = 0;
  } else {
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
    LOGW("PeerConnection is NULL");
    return -1;
  } else if (g_ps.http_port <= 0) {
    LOGW("Invalid HTTP port number: %d", g_ps.http_port);
    return -1;
  }
  printf("Inside the peer_signalling_whep_connect");
  peer_connection_create_offer(g_ps.pc);
  return 0;
}


void peer_signaling_whip_disconnect() {
  // TODO: implement
}

int peer_signaling_join_channel() {
  if (g_ps.pc == NULL) {
    LOGW("PeerConnection is NULL");
    return -1;
  } else if (g_ps.mqtt_port <= 0) {
    LOGW("Invalid MQTT port number: %d", g_ps.mqtt_port);
    if (peer_signaling_whip_connect() < 0) {
      LOGW("Tried MQTT and WHIP, connect failed");
      return -1;
    }
    return 0;
  }

  if (peer_signaling_mqtt_connect(g_ps.mqtt_host, g_ps.mqtt_port) < 0) {
    LOGW("Connect MQTT server failed");
    return -1;
  }

  peer_signaling_mqtt_subscribe(1);
  return 0;
}

int peer_signaling_loop() {
  if (g_ps.mqtt_port > 0) {
    MQTT_ProcessLoop(&g_ps.mqtt_ctx);
  }
  return 0;
}

void peer_signaling_leave_channel() {
  MQTTStatus_t status = MQTTSuccess;

  if (g_ps.mqtt_port > 0 && peer_signaling_mqtt_subscribe(0) == 0) {
    status = MQTT_Disconnect(&g_ps.mqtt_ctx);
    if (status != MQTTSuccess) {
      LOGE("Failed to disconnect with broker: %s", MQTT_Status_strerror(status));
    }
  }
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

    printf("HTTP Host: %s, Port: %d, Path: %s", g_ps.http_host, g_ps.http_port, g_ps.http_path);
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
