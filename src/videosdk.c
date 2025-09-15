#include "videosdk.h"
#include <core_http_client.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "peer.h"
#include "peer_connection.h"
#include "peer_signaling.h"
#include "ssl_transport.h"
bool init_check = false;
#define MAX_HTTP_OUTPUT_BUFFER 512
static const char* TAG = "videosdk";
extern int init_board(void);
extern esp_err_t audio_codec_init();
char* g_meetingId;
char* g_token_videosdk;
char* g_displayName;
char* g_participantId;
char* g_publisherId;
char* g_subscriberId;
audio_codec_t g_codec;
PeerConnection* g_pc_publish;
PeerConnection* g_pc_subscribe;
PeerConnectionState eState = PEER_CONNECTION_CLOSED;
int publish_task = 0;
int subscribe_Task = 0;
SemaphoreHandle_t xSemaphore_publish = NULL;
SemaphoreHandle_t xSemaphore_subscribe = NULL;
static TaskHandle_t xPcTaskHandlePublish = NULL;
static TaskHandle_t xPcTaskHandleSubscribe = NULL;
static TaskHandle_t xAudioTaskHandle = NULL;
static TaskHandle_t xSubscribeAudioTaskHandle = NULL;
const char* g_videosdk_hostname = "dev-api.videosdk.live";
const char* g_http_url = "dev-api.videosdk.live";
extern void audio_deinit(void);
extern esp_err_t audio_av_render_init(audio_codec_t codec);
extern void audio_receive_and_render(const uint8_t* encoded_data, size_t encoded_len, uint32_t timestamp);
extern void audio_task(void* pvParameters);
extern void generate_random_string(char* str, int length);
extern bool is_null_or_empty(const char* str);

// structure for the peerconnection task
typedef struct {
  PeerConnection* pc;
} peer_connection_task_t;

char* get_meetingId() {
  return g_meetingId;
}

char* get_displayName() {
  return g_displayName;
}
audio_codec_t get_codec() {
  return g_codec;
}

char* get_publisherId() {
  return g_publisherId;
}

char* get_subscriberId() {
  return g_subscriberId;
}

// crate Meeting Function
create_meeting_result_t create_meeting(char* token) {
  TransportInterface_t trans_if = {0};
  NetworkContext_t net_ctx;
  HTTPResponse_t res;
  char* body = malloc(256);
  int ret;

  if (!body) {
    return (create_meeting_result_t){MEMORY_ALLOC_FAILED, NULL};
  }

  trans_if.recv = ssl_transport_recv;
  trans_if.send = ssl_transport_send;
  trans_if.pNetworkContext = &net_ctx;

  ret = ssl_transport_connect(&net_ctx, "dev-api.videosdk.live", 443, NULL);
  if (ret < 0) {
    free(body);
    return (create_meeting_result_t){SSL_CONNECT_FAILED, NULL};
  }

  snprintf(body, 256, "{}");

  res = peer_signaling_http_request(
      &trans_if,
      "POST", strlen("POST"),
      "dev-api.videosdk.live", strlen("dev-api.videosdk.live"),
      "/v2/rooms", strlen("/v2/rooms"),
      token, strlen(token),
      body, strlen(body));

  free(body);
  ssl_transport_disconnect(&net_ctx);

  if (res.pBody == NULL || res.statusCode != 200) {
    ESP_LOGE(TAG, "Create meeting failed. HTTP Status: %u", res.statusCode);
    return (create_meeting_result_t){HTTP_REQUEST_FAILED, NULL};
  }
  const char* body_str = (const char*)res.pBody;
  char* room_id_ptr = strstr(body_str, "\"roomId\":\"");

  room_id_ptr += strlen("\"roomId\":\"");
  char* end_quote = strchr(room_id_ptr, '"');

  size_t room_id_len = end_quote - room_id_ptr;
  char* room_id = (char*)malloc(room_id_len + 1);
  if (!room_id) {
    return (create_meeting_result_t){MEMORY_ALLOC_FAILED, NULL};
  }
  strncpy(room_id, room_id_ptr, room_id_len);
  room_id[room_id_len] = '\0';

  return (create_meeting_result_t){RESULT_OK, room_id};
}

// thread scheduler used inside the init function for memory allocation
static void thread_scheduler(const char* thread_name, media_lib_thread_cfg_t* thread_cfg) {
  if (strcmp(thread_name, "pc_task") == 0) {
    thread_cfg->stack_size = 25 * 1024;
    thread_cfg->priority = 18;
    thread_cfg->core_id = 1;
  }
  if (strcmp(thread_name, "start") == 0) {
    thread_cfg->stack_size = 6 * 1024;
  }
  if (strcmp(thread_name, "pc_send") == 0) {
    thread_cfg->stack_size = 4 * 1024;
    thread_cfg->priority = 15;
    thread_cfg->core_id = 1;
  }
  if (strcmp(thread_name, "Adec") == 0) {
    thread_cfg->stack_size = 40 * 1024;
    thread_cfg->priority = 10;
    thread_cfg->core_id = 1;
  }
  if (strcmp(thread_name, "venc") == 0) {
    thread_cfg->stack_size = 20 * 1024;
    thread_cfg->priority = 10;
  }
#ifdef WEBRTC_SUPPORT_OPUS
  if (strcmp(thread_name, "aenc") == 0) {
    thread_cfg->stack_size = 40 * 1024;
    thread_cfg->priority = 10;
  }
  if (strcmp(thread_name, "SrcRead") == 0) {
    thread_cfg->stack_size = 40 * 1024;
    thread_cfg->priority = 16;
    thread_cfg->core_id = 0;
  }
  if (strcmp(thread_name, "buffer_in") == 0) {
    thread_cfg->stack_size = 6 * 1024;
    thread_cfg->priority = 10;
    thread_cfg->core_id = 0;
  }
#endif
}

// init function - for taking the info from the user
result_t init(init_config_t* cfg) {
  // Check for required parameters
  if (cfg->meetingID == NULL || cfg->token == NULL) {
    ESP_LOGE(TAG, "Meeting ID or token is NULL");
    return NULL_PARAMETER;
  }
  g_meetingId = cfg->meetingID;
  g_token_videosdk = cfg->token;
  g_codec = cfg->audioCodec;

  // Used for adding the default adapter used in auido_codec_init.
  media_lib_add_default_adapter();
  // for allocation for the memory
  media_lib_thread_set_schedule_cb(thread_scheduler);

  // Handle displayName - generate random 8-character string if null/empty
  if (is_null_or_empty(cfg->displayName)) {
    srand((unsigned int)time(NULL));
    static char generated_display_name[9];
    generate_random_string(generated_display_name, 8);
    g_displayName = generated_display_name;
    ESP_LOGI(TAG, "Generated displayName: %s", g_displayName);
  } else {
    g_displayName = cfg->displayName;
  }

  init_check = true;
// init_board for the korovo v2 esp only
#if defined(CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD)
  if (init_board() != 0) {
    ESP_LOGE(TAG, "Board initialization failed");
    return INIT_BOARD_FAILED;
  }
#endif
  // peer_init for srtp_init()
  if (peer_init() != 0) {
    ESP_LOGE(TAG, "Peer initialization failed");
    return PEER_INIT_FAILED;
  }

  return RESULT_OK;
}

// state for publish
static void oniceconnectionstatechangePublish(PeerConnectionState state, void* user_data) {
  eState = state;

  switch (state) {
    case PEER_CONNECTION_FAILED:
      stopPublishAudio();
      break;
    case PEER_CONNECTION_CLOSED:
      stopPublishAudio();
      break;
    default:
      break;
  }
}

// state for subscribe
static void oniceconnectionstatechangeSubscribe(PeerConnectionState state, void* user_data) {
  eState = state;

  switch (state) {
    case PEER_CONNECTION_FAILED:
      stopSubscribeAudio();
      break;
    case PEER_CONNECTION_CLOSED:
      stopSubscribeAudio();
      break;
    default:
      break;
  }
}

// peer connection task for the publish
void peer_connection_task_publish() {
  for (;;) {
    if (xSemaphoreTake(xSemaphore_publish, portMAX_DELAY)) {
      peer_connection_loop(g_pc_publish);
      xSemaphoreGive(xSemaphore_publish);
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
// peer connection task for the subscribe
void peer_connection_task_subscribe() {
  ESP_LOGI(TAG, "peer_connection_task started");

  for (;;) {
    if (xSemaphoreTake(xSemaphore_subscribe, portMAX_DELAY)) {
      peer_connection_loop(g_pc_subscribe);
      xSemaphoreGive(xSemaphore_subscribe);
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// WHEP
char path_buffer_subsribe[256];
result_t startSubscribeAudio(char* subscriberId, char* subscribeToId) {
// XIAO Check
#if CONFIG_ESP32S3_XIAO
  return DEVICE_NOT_SUPPORTED;
#endif
  if (get_subscribe_check()) {
    ESP_LOGE(TAG, "Subscribe Task Already Started.");
    return TASK_ALREADY_STARTED;
  }

  if (!init_check) {
    ESP_LOGE(TAG, "INIT Method Called.");
    return INIT_NOT_CALLED;
  }

  if (is_null_or_empty(subscribeToId)) {
    return NULL_PARAMETER;
  }

  set_subscribe_check(true);
  if (is_null_or_empty(subscriberId)) {
    static char generated_subscriberId_name[9];
    generate_random_string(generated_subscriberId_name, 8);
    subscriberId = generated_subscriberId_name;
    g_subscriberId = subscriberId;
    ESP_LOGI(TAG, "Generated displayName: %s", subscriberId);
  } else {
    g_subscriberId = subscriberId;
  }
  if (subscriberId == g_publisherId) {
    ESP_LOGE(TAG, "Paticipant Id can't be same.");
    return DUPLICATE_ID;
  }
  // start with av_render_init
  xSemaphore_subscribe = xSemaphoreCreateMutex();
  if (xSemaphore_subscribe == NULL) {
    ESP_LOGE(TAG, "Failed to create subscribe mutex");
    set_subscribe_check(false);
    return SUBSCRIBE_MUTEX_CREATE_FAILED;
  }
  // av render init for play the audio
  int ret = audio_av_render_init(g_codec);
  if (ret < 0) {
    ESP_LOGE(TAG, "AV render initialization failed");
    set_subscribe_check(false);
    return AUDIO_CODEC_INIT_FAILED;
  }
  /// @brief  setting the peer connection config
  PeerConfiguration config = {
      .ice_servers = {
          {.urls = "stun:stun.l.google.com:19302"}}};
  config.onaudiotrack = audio_receive_and_render;
  switch (g_codec) {
    case AUDIO_CODEC_OPUS:
      config.audio_codec = CODEC_OPUS;
      break;
    case AUDIO_CODEC_PCMA:
      config.audio_codec = CODEC_PCMA;
      break;
    case AUDIO_CODEC_PCMU:
      config.audio_codec = CODEC_PCMU;
      break;
    default:
      config.audio_codec = CODEC_OPUS;
      break;
  }
  // creating the peerconnection
  g_pc_subscribe = peer_connection_create(&config, false);
  if (!g_pc_subscribe) {
    ESP_LOGE(TAG, "Failed to create subscribe peer connection");
    set_subscribe_check(false);
    return SUBSCRIBE_PEER_CONNECTION_FAILED;
  }
  // state allocation and also the https request config
  peer_connection_oniceconnectionstatechange(g_pc_subscribe, oniceconnectionstatechangeSubscribe);
  ServiceConfiguration service_config = SERVICE_CONFIG_DEFAULT();
  service_config.pc = g_pc_subscribe;
  service_config.hostname = g_videosdk_hostname;
  snprintf(path_buffer_subsribe, sizeof(path_buffer_subsribe), "/v2/whep?roomId=%s&participantId=%s&remotePeerId=%s", g_meetingId, subscriberId, subscribeToId);
  service_config.path = strdup(path_buffer_subsribe);
  service_config.http_url = g_http_url;
  service_config.http_port = 443;
  service_config.auth_token = g_token_videosdk;
  peer_signaling_set_config(&service_config);
  // whep connect request for create offer
  peer_signaling_whep_connect();

  // peer connection task - subscribe
  StaticTask_t* pc_task_buffer = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
  StackType_t* pc_stack = heap_caps_malloc(16384 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
  peer_connection_task_t* task_args = heap_caps_malloc(sizeof(peer_connection_task_t), MALLOC_CAP_INTERNAL);
  if (pc_task_buffer && pc_stack && task_args) {
    task_args->pc = g_pc_subscribe;
    xPcTaskHandleSubscribe = xTaskCreateStaticPinnedToCore(peer_connection_task_subscribe, "peer_connection", 16384, NULL, 5, pc_stack, pc_task_buffer, 1);
    if (xPcTaskHandleSubscribe == NULL) {
      ESP_LOGE(TAG, "Failed to create peer connection task");
      set_subscribe_check(false);
      return SUBSCRIBE_TASK_CREATE_FAILED;
    }
  } else {
    ESP_LOGE(TAG, "Failed to allocate memory for peer_connection task");
    set_subscribe_check(false);
    return SUBSCRIBE_MEMORY_ALLOC_FAILED;
  }

  return RESULT_OK;
}
// WHIP
char path_buffer_publish[256];
result_t startPublishAudio(char* publishId) {
  printf("insid the publish audio");

  if (get_publish_check()) {
    ESP_LOGE(TAG, "Publish Task Already running");
    return TASK_ALREADY_STARTED;
  }
  if (!init_check) {
    ESP_LOGE(TAG, "INIT methood not Called");
    return INIT_NOT_CALLED;
  }

  set_publish_check(true);
  if (is_null_or_empty(publishId)) {
    static char generated_publishId_name[9];
    generate_random_string(generated_publishId_name, 8);
    publishId = generated_publishId_name;
    g_publisherId = publishId;
    ESP_LOGI(TAG, "Generated displayName: %s", publishId);
  } else {
    g_publisherId = publishId;
  }
  if (publishId == g_subscriberId) {
    ESP_LOGE(TAG, "Paticipant Id can't be same.");
    return DUPLICATE_ID;
  }

  if (xSemaphore_publish == NULL) {
    xSemaphore_publish = xSemaphoreCreateMutex();
    if (xSemaphore_publish == NULL) {
      ESP_LOGE(TAG, "Failed to create mutex");
      set_publish_check(false);
      return PUBLISH_MUTEX_CREATE_FAILED;
    }
  }
  // audio codec init to send audio data
  int ret = audio_codec_init(g_codec);
  if (ret < 0) {
    ESP_LOGE(TAG, "Audio codec initialization failed");
    set_publish_check(false);
    return AUDIO_CODEC_INIT_FAILED;
  }

  // peer connection config

  PeerConfiguration config = {
      .ice_servers = {
          {.urls = "stun:stun.l.google.com:19302"}}};

  switch (g_codec) {
    case AUDIO_CODEC_OPUS:
      config.audio_codec = CODEC_OPUS;
      break;
    case AUDIO_CODEC_PCMA:
      config.audio_codec = CODEC_PCMA;
      break;
    case AUDIO_CODEC_PCMU:
      config.audio_codec = CODEC_PCMU;
      break;
    default:
      config.audio_codec = CODEC_OPUS;
      break;
  }
  // create peer connection task
  g_pc_publish = peer_connection_create(&config, true);
  if (!g_pc_publish) {
    ESP_LOGE(TAG, "Failed to create peer connection");
    set_publish_check(false);
    return PUBLISH_PEER_CONNECTION_FAILED;
  }

  peer_connection_oniceconnectionstatechange(g_pc_publish, oniceconnectionstatechangePublish);

  ServiceConfiguration service_config = SERVICE_CONFIG_DEFAULT();
  service_config.pc = g_pc_publish;
  service_config.hostname = g_videosdk_hostname;
  snprintf(path_buffer_publish, sizeof(path_buffer_publish), "/v2/whip?roomId=%s&participantId=%s&displayName=%s", g_meetingId, publishId, g_displayName);
  service_config.path = strdup(path_buffer_publish);
  service_config.http_url = g_http_url;
  service_config.http_port = 443;
  service_config.auth_token = g_token_videosdk;
  peer_signaling_set_config(&service_config);

  // whip connect request for create offer
  peer_signaling_whip_connect();
  // audio task to record audio and send to the server
  StaticTask_t* audio_task_buffer = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
  StackType_t* audio_stack = heap_caps_malloc(20480 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
  if (audio_task_buffer && audio_stack) {
    xAudioTaskHandle = xTaskCreateStaticPinnedToCore(audio_task, "audio", 20480, NULL, 9, audio_stack, audio_task_buffer, 0);
    if (xAudioTaskHandle == NULL) {
      ESP_LOGE(TAG, "Failed to create audio task");
      set_publish_check(false);
      return PUBLISH_TASK_CREATE_FAILED;
    }
  } else {
    ESP_LOGE(TAG, "Failed to allocate memory for audio task");
    set_publish_check(false);
    return PUBLISH_MEMORY_ALLOC_FAILED;
  }
  // peer connection task - publish
  StaticTask_t* pc_task_buffer = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
  StackType_t* pc_stack = heap_caps_malloc(16384 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
  peer_connection_task_t* task_args = heap_caps_malloc(sizeof(peer_connection_task_t), MALLOC_CAP_INTERNAL);
  if (pc_task_buffer && pc_stack && task_args) {
    task_args->pc = g_pc_publish;
    xPcTaskHandlePublish = xTaskCreateStaticPinnedToCore(peer_connection_task_publish, "peer_connection", 16384, NULL, 5, pc_stack, pc_task_buffer, 1);
    if (xPcTaskHandlePublish == NULL) {
      ESP_LOGE(TAG, "Failed to create peer connection task");
      set_publish_check(false);
      return PUBLISH_TASK_CREATE_FAILED;
    }
  } else {
    ESP_LOGE(TAG, "Failed to allocate memory for peer_connection task");
    set_publish_check(false);
    return PUBLISH_MEMORY_ALLOC_FAILED;
  }

  while (eState != PEER_CONNECTION_COMPLETED && eState != PEER_CONNECTION_FAILED) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  return RESULT_OK;
}
void stop_publish_task(void* param) {
  if (get_publish_check()) {
    int ret = delete_publish_peer_from_meeting();

    if (ret == 0) {
      if (xAudioTaskHandle != NULL) {
        vTaskSuspend(xAudioTaskHandle);
      }
      if (xPcTaskHandlePublish != NULL) {
        vTaskSuspend(xPcTaskHandlePublish);
      }

      peer_deinit();

      set_publish_check(false);
    }

    vTaskDelete(NULL);
  } else {
    ESP_LOGE(TAG, "Publish Task Not Started.");
    vTaskDelete(NULL);
  }
}
result_t stopPublishAudio() {
  if (xTaskCreatePinnedToCore(stop_publish_task, "stop_publish_task", 8192, NULL, 5, NULL, 0) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create stop publish task");
    return STOP_PUBLISH_TASK_CREATE_FAILED;
  }
  return RESULT_OK;
}

void stop_subscribe_task(void* param) {
  if (get_subscribe_check()) {
    int ret = delete_subscribe_peer_from_meeting();
    if (ret == 0) {
      if (xPcTaskHandleSubscribe != NULL) {
        vTaskSuspend(xPcTaskHandleSubscribe);
      }

      peer_deinit();

      set_subscribe_check(false);
    }

    vTaskDelete(NULL);
  } else {
    ESP_LOGE(TAG, "Subscribe Task not Started.");
    vTaskDelete(NULL);
  }
}

result_t stopSubscribeAudio() {
  if (xTaskCreatePinnedToCore(stop_subscribe_task, "stop_subscribe_task", 8192, NULL, 5, NULL, 0) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create stop publish task");
    return STOP_PUBLISH_TASK_CREATE_FAILED;
  }
  return RESULT_OK;
}

result_t leave() {
  result_t result_publish = stopPublishAudio();
  if (result_publish != RESULT_OK) {
    ESP_LOGE(TAG, "Stop Publish Task Failed.");
    return LEAVE_FAILED;
  }
  result_t result_subscribe = stopSubscribeAudio();
  if (result_subscribe != RESULT_OK) {
    ESP_LOGE(TAG, "Stop Subscribe Task Failed.");
    return LEAVE_FAILED;
  }

  return RESULT_OK;
}
