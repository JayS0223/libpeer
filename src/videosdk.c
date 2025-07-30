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
#include "peer.h"
#include "videosdk.h"
#include "peer_signaling.h"
#include "ssl_transport.h"
#include "peer_connection.h"




#define MAX_HTTP_OUTPUT_BUFFER 512
static const char* TAG = "videosdk";
extern void init_board(void);
extern esp_err_t audio_codec_init();
char *g_meetingId;
char *g_token_videosdk;
char *g_displayName;
 PeerConnection* g_pc_publish;
 PeerConnection* g_pc_subscribe;
 PeerConnectionState eState = PEER_CONNECTION_CLOSED;
SemaphoreHandle_t xSemaphore = NULL;
static TaskHandle_t xPcTaskHandle = NULL;
static TaskHandle_t xAudioTaskHandle = NULL;
static TaskHandle_t xSubscribeAudioTaskHandle = NULL;
extern esp_err_t audio_av_render_init();
extern void audio_receive_and_render(const uint8_t* encoded_data, size_t encoded_len, uint32_t timestamp);
extern void audio_task(void* pvParameters);
void startSubscribeAudioTask(void *arg); // Forward declaration
int64_t get_timestamp_videosdk() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL));
}

typedef struct {
    PeerConnection* pc;
} peer_connection_task_t;

// char* create_meeting(const char *auth_token) {
//   TransportInterface_t trans_if = {0};
//   NetworkContext_t net_ctx;
//   HTTPResponse_t res;

//   const char *body = "";  // Empty body for POST
//   int ret;

//   trans_if.recv = ssl_transport_recv;
//   trans_if.send = ssl_transport_send;
//   trans_if.pNetworkContext = &net_ctx;

//   ret = ssl_transport_connect(&net_ctx, API_HOST, API_PORT, NULL);
//   if (ret < 0) {
//     ESP_LOGE(TAG, "Connection failed to %s:%d", API_HOST, API_PORT);
//     return NULL;
//   }

//   ESP_LOGI(TAG, "Calling VideoSDK /v2/rooms...");
//   res = peer_signaling_http_request(
//     &trans_if,
//     "POST", strlen("POST"),
//     API_HOST, strlen(API_HOST),
//     "/v2/rooms", strlen("/v2/rooms"),
//     auth_token, strlen(auth_token),
//     body, strlen(body)
//   );

//   ssl_transport_disconnect(&net_ctx);

//   if (res.pBody == NULL || res.statusCode != 200) {
//     ESP_LOGE(TAG, "Create meeting failed. HTTP Status: %u", res.statusCode);
//     return NULL;
//   }

//   ESP_LOGI(TAG, "Response: %s", res.pBody);

//   // Simple extraction of "roomId" from JSON response
//   // Assumes format: {"roomId":"abc123"} (naive parser for demo)
// const char *body_str = (const char *)res.pBody;
// char *room_id_ptr = strstr(body_str, "\"roomId\":\"");
//   if (!room_id_ptr) return NULL;

//   room_id_ptr += strlen("\"roomId\":\""); // Move past the key
//   char *end_quote = strchr(room_id_ptr, '"');
//   if (!end_quote) return NULL;

//   size_t room_id_len = end_quote - room_id_ptr;
//   char *room_id = (char *)malloc(room_id_len + 1);
//   strncpy(room_id, room_id_ptr, room_id_len);
//   room_id[room_id_len] = '\0';

//   return room_id;
// }

int init(init_config_t *cfg){
    g_meetingId = cfg->meetingID;
    g_token_videosdk = cfg->token;
    g_displayName = cfg->displayName;
    if (g_meetingId == NULL || g_token_videosdk == NULL || g_displayName == NULL) {
        ESP_LOGE(TAG, "Meeting ID, token or display name is NULL");
        return ESP_FAIL;
    }
    // add the error args here
    init_board();
     peer_init();
return 0;
}



static void oniceconnectionstatechange(PeerConnectionState state, void* user_data) {
  ESP_LOGI(TAG, "PeerConnectionState changed: %d (%s)", state, peer_connection_state_to_string(state));
  eState = state;

  // if (on_connection_state_changed_cb) {
  //   on_connection_state_changed_cb(state);  // Invoke the user-defined callback
  // }

  switch (state) {
    case PEER_CONNECTION_CONNECTED:
      ESP_LOGI(TAG, "DTLS handshake completed, connection is now CONNECTED");
      break;
    case PEER_CONNECTION_COMPLETED:
      ESP_LOGI(TAG, "ICE and DTLS completed, connection is now COMPLETED");
      break;
    case PEER_CONNECTION_FAILED:
      ESP_LOGE(TAG, "PeerConnection FAILED");
      break;
    case PEER_CONNECTION_CLOSED:
      ESP_LOGW(TAG, "PeerConnection CLOSED");
      break;
    default:
      break;
  }
}

void peer_connection_task(void* arg) {
  ESP_LOGI(TAG, "peer_connection_task started");

  peer_connection_task_t* task_data = (peer_connection_task_t*) arg;
PeerConnection* pc = task_data->pc;
  for (;;) {
    int64_t ts = get_timestamp_videosdk();
    printf("[%lld ms] peer_connection_loop publish", ts);
    if (xSemaphoreTake(xSemaphore, portMAX_DELAY)) {
      peer_connection_loop(pc);
      xSemaphoreGive(xSemaphore);
    }
    
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}



// int stopPublishAudio(){
// audio_deinit();
//}
int startSubscribeAudio(audio_codec_t cfg){
  // start with av_render_init
   xSemaphore = xSemaphoreCreateMutex();
   int ret =  audio_av_render_init();
   if(ret >= 0){
    ESP_LOGI(TAG, " AV render initialization successful");
   }else {
    ESP_LOGE(TAG, "AV render initialization failed");
    return -1;
   }
// peer_signaling
PeerConfiguration config = {
    .ice_servers = {
        {.urls = "stun:stun.l.google.com:19302"
        }},
   .audio_codec = CODEC_OPUS,
   .onaudiotrack = audio_receive_and_render,
  };

   g_pc_subscribe = peer_connection_create(&config, false, true);
   peer_connection_oniceconnectionstatechange(g_pc_subscribe, oniceconnectionstatechange);
 ServiceConfiguration service_config = SERVICE_CONFIG_DEFAULT(); 
  service_config.pc = g_pc_subscribe;
  service_config.hostname = "dev-whip.videosdk.live";
  service_config.path = "/whep";
  service_config.http_url = "dev-whip.videosdk.live";
  service_config.http_port = 443;
  service_config.auth_token = g_token_videosdk;
  // set service config
  printf("Setting service configuration: %s\n", service_config.auth_token);
   peer_signaling_set_config(&service_config);
// whep connect request for create offer
  peer_signaling_whep_connect();  
  peer_connection_task_t* task_args = malloc(sizeof(peer_connection_task_t));

  task_args->pc = g_pc_subscribe;
//  peer_connection_task(task_args);
 xTaskCreatePinnedToCore(peer_connection_task, "peer_connection", 16834, task_args, 5, &xPcTaskHandle, 1);
//  xTaskCreate(
//     peer_connection_task,   // Task function
//     "peer_connection",      // Name for debugging
//     16834,                  // Stack size (in words)
//     task_args,              // Argument to task
//     5,                      // Priority
//     &xPcTaskHandle          // Task handle
// );
 while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
 return 0;
}

void startSubscribeAudioTask(void *arg) {
  ESP_LOGI(TAG, "Starting subscribe audio task");
  int ret = startSubscribeAudio(AUDIO_CODEC_OPUS);
  if (ret < 0) {
    ESP_LOGE(TAG, "Failed to start subscribe audio");
  }
}



// int stopSubscribeAudio(){
// audio_deinit();

// }

int startPublishAudio(audio_codec_t cfg) {
printf("Inside the startPulish Function");
// start with audio_codec_init
   xSemaphore = xSemaphoreCreateMutex();
   int ret =  audio_codec_init();
   if(ret >= 0){
    ESP_LOGI(TAG, "Audio codec initialization successful");
   }else {
    ESP_LOGE(TAG, "Audio codec initialization failed");
    return -1;
   }
// peer_signaling
PeerConfiguration config = {
    .ice_servers = {
        {.urls = "stun:stun.l.google.com:19302"
        }},
   .audio_codec = CODEC_PCMA,
  };

   g_pc_publish = peer_connection_create(&config, true, false);
   peer_connection_oniceconnectionstatechange(g_pc_publish, oniceconnectionstatechange);
 ServiceConfiguration service_config = SERVICE_CONFIG_DEFAULT(); 
  service_config.pc = g_pc_publish;
  service_config.hostname = "dev-api.videosdk.live";
  service_config.path = "/v2/whip?roomId=roye-pqdd-wbfl&participantId=whip-peer";
  service_config.http_url = "dev-api.videosdk.live";
  service_config.http_port = 443;
  service_config.auth_token = g_token_videosdk;
  // set service config
  printf("Setting service configuration: %s\n", service_config.auth_token);
   peer_signaling_set_config(&service_config);
// whip connect request for create offer
  peer_signaling_whip_connect();

// add the audio task 
   StackType_t* stack_memory = (StackType_t*)heap_caps_malloc(16384 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
  StaticTask_t task_buffer;
  if (stack_memory) {
    xAudioTaskHandle = xTaskCreateStaticPinnedToCore(audio_task, "audio", 8192, NULL, 9, stack_memory, &task_buffer, 0);
  }
    peer_connection_task_t* task_args = malloc(sizeof(peer_connection_task_t));

  task_args->pc = g_pc_publish;
xTaskCreatePinnedToCore(peer_connection_task, "peer_connection", 16834, task_args, 5, &xPcTaskHandle, 1);
  while (1) {
   // printf("Time called %lld ms\n", get_timestamp_videosdk());
   // printf("Waiting for audio task to complete...\n");
    vTaskDelay(pdMS_TO_TICKS(10));
  }
return 0;
}