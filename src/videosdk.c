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
#include <stdbool.h>
#include <core_http_client.h>

#define MAX_HTTP_OUTPUT_BUFFER 512
static const char* TAG = "videosdk";
extern int init_board(void);
extern esp_err_t audio_codec_init();
char *g_meetingId;
char *g_token_videosdk;
char *g_displayName;
char *g_participantId;
 PeerConnection* g_pc_publish;
 PeerConnection* g_pc_subscribe;
 PeerConnectionState eState = PEER_CONNECTION_CLOSED;
SemaphoreHandle_t xSemaphore_publish = NULL;
SemaphoreHandle_t xSemaphore_subscribe = NULL;
static TaskHandle_t xPcTaskHandlePublish = NULL;
static TaskHandle_t xPcTaskHandleSubscribe = NULL;
static TaskHandle_t xAudioTaskHandle = NULL;
static TaskHandle_t xSubscribeAudioTaskHandle = NULL;
extern void audio_deinit(void);
extern esp_err_t audio_av_render_init();
extern void audio_receive_and_render(const uint8_t* encoded_data, size_t encoded_len, uint32_t timestamp);
extern void audio_task(void* pvParameters);
extern void removePeer();
extern void generate_random_string(char *str, int length);
extern bool is_null_or_empty(const char *str);
int64_t get_timestamp_videosdk() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL));
}

typedef struct {
    PeerConnection* pc;
} peer_connection_task_t;


create_meeting_result_t create_meeting(create_meeting_config_t *meetingConfig_t) {
    TransportInterface_t trans_if = {0};
    NetworkContext_t net_ctx;
    HTTPResponse_t res;
    char *body = malloc(256);
    int ret;

    if (!body) {
        return (create_meeting_result_t){ MEMORY_ALLOC_FAILED, NULL };
    }

    trans_if.recv = ssl_transport_recv;
    trans_if.send = ssl_transport_send;
    trans_if.pNetworkContext = &net_ctx;

    
    ret = ssl_transport_connect(&net_ctx, "dev-api.videosdk.live", 443, NULL);
    if (ret < 0) {
        free(body);
        return (create_meeting_result_t){ SSL_CONNECT_FAILED, NULL };
    }

    snprintf(body, 256, "{}");
    
    res = peer_signaling_http_request(
        &trans_if,
        "POST", strlen("POST"),
        "dev-api.videosdk.live", strlen("dev-api.videosdk.live"),
        "/v2/rooms", strlen("/v2/rooms"),
        meetingConfig_t->token, strlen(meetingConfig_t->token),
        body, strlen(body)
    );

    free(body);
    ssl_transport_disconnect(&net_ctx);

    if (res.pBody == NULL || res.statusCode != 200) {
        ESP_LOGE(TAG, "Create meeting failed. HTTP Status: %u", res.statusCode);
        return (create_meeting_result_t){ HTTP_REQUEST_FAILED, NULL };
    }
    const char *body_str = (const char *)res.pBody;
    char *room_id_ptr = strstr(body_str, "\"roomId\":\"");
 
    room_id_ptr += strlen("\"roomId\":\"");
    char *end_quote = strchr(room_id_ptr, '"');

    size_t room_id_len = end_quote - room_id_ptr;
    char *room_id = (char *)malloc(room_id_len + 1);
    if (!room_id) {
        return (create_meeting_result_t){ MEMORY_ALLOC_FAILED, NULL };
    }
    strncpy(room_id, room_id_ptr, room_id_len);
    room_id[room_id_len] = '\0';

    return (create_meeting_result_t){ RESULT_OK, room_id };
}

// result_t init(init_config_t *cfg) {
//     g_meetingId = cfg->meetingID;
//     g_token_videosdk = cfg->token;
//     g_displayName = cfg->displayName;
//     g_participantId = cfg->participantId;
//     if (g_meetingId == NULL || g_token_videosdk == NULL || g_displayName == NULL) {
//         // ESP_LOGE(TAG, "Meeting ID, token or display name is NULL");
//         return INIT_NULL_PARAMETER;
//     }
    
//     // add the error args here
//     #if defined(CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD)
//     if (init_board() != 0) {
//         ESP_LOGE(TAG, "Board initialization failed");
//         return INIT_BOARD_FAILED;
//     }
//     #endif
    
//     if (peer_init() != 0) {
//         ESP_LOGE(TAG, "Peer initialization failed");
//         return INIT_PEER_FAILED;
//     }
    
//     return RESULT_OK;
// }

result_t init(init_config_t *cfg) {
    // Initialize random seed (should be done once in your application)
    static bool rand_initialized = false;
    if (!rand_initialized) {
        srand((unsigned int)time(NULL));
        rand_initialized = true;
    }
    
    g_meetingId = cfg->meetingID;
    g_token_videosdk = cfg->token;
    
    // Handle participantId - generate random 6-character string if null/empty
    if (is_null_or_empty(cfg->participantId)) {
        static char generated_participant_id[7]; // 6 chars + null terminator
        generate_random_string(generated_participant_id, 6);
        g_participantId = generated_participant_id;
        ESP_LOGI(TAG, "Generated participantId: %s", g_participantId);
    } else {
        g_participantId = cfg->participantId;
    }
    
    // Handle displayName - generate random 8-character string if null/empty
    if (is_null_or_empty(cfg->displayName)) {
        static char generated_display_name[9]; // 8 chars + null terminator
        generate_random_string(generated_display_name, 8);
        g_displayName = generated_display_name;
        ESP_LOGI(TAG, "Generated displayName: %s", g_displayName);
    } else {
        g_displayName = cfg->displayName;
    }
    
    // Check for required parameters
    if (g_meetingId == NULL || g_token_videosdk == NULL) {
        ESP_LOGE(TAG, "Meeting ID or token is NULL");
        return INIT_NULL_PARAMETER;
    }
    
    #if defined(CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD)
    if (init_board() != 0) {
        ESP_LOGE(TAG, "Board initialization failed");
        return INIT_BOARD_FAILED;
    }
    #endif
    
    if (peer_init() != 0) {
        ESP_LOGE(TAG, "Peer initialization failed");
        return INIT_PEER_FAILED;
    }
    
    return RESULT_OK;
}

void loop_log(){
  ESP_LOGI(TAG, "Loop log started");
  for(int i = 0; i < 10; i++) {
    ESP_LOGI(TAG, "Loop iteration: %d", i);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Delay for 1 second
  }
}


static void oniceconnectionstatechangePublish(PeerConnectionState state, void* user_data) {
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
      stopPublishAudio(); 
      break;
    case PEER_CONNECTION_CLOSED:
      ESP_LOGW(TAG, "PeerConnection CLOSED");
   //   stopPublishAudio();
      break;
    default:
      break;
  }
}

static void oniceconnectionstatechangeSubscribe(PeerConnectionState state, void* user_data) {
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
      if (xPcTaskHandleSubscribe != NULL) {
        vTaskSuspend(xPcTaskHandleSubscribe);
        ESP_LOGI(TAG, "Peer connection task suspended (subscribe)");
      }
      break;
    case PEER_CONNECTION_CLOSED:
      ESP_LOGW(TAG, "PeerConnection CLOSED");
      if (xPcTaskHandleSubscribe != NULL) {
        vTaskSuspend(xPcTaskHandleSubscribe);
        ESP_LOGI(TAG, "Peer connection task suspended (subscribe)");
      }
      break;
    default:
      break;
  }
}

void peer_connection_task_publish() {
  ESP_LOGI(TAG, "peer_connection_task_publish started");

  for (;;) {
    int64_t ts = get_timestamp_videosdk();
    printf("[%lld ms] peer_connection_loop publish", ts);
    if (xSemaphoreTake(xSemaphore_publish, portMAX_DELAY)) {
      peer_connection_loop(g_pc_publish);
      xSemaphoreGive(xSemaphore_publish);
    }
    
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
void peer_connection_task_subscribe() {
  ESP_LOGI(TAG, "peer_connection_task started");

  for (;;) {
    int64_t ts = get_timestamp_videosdk();
    printf("[%lld ms] peer_connection_loop publish", ts);
    if (xSemaphoreTake(xSemaphore_subscribe, portMAX_DELAY)) {
      peer_connection_loop(g_pc_subscribe);
      xSemaphoreGive(xSemaphore_subscribe);
    }
    
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}


result_t startSubscribeAudio(audio_codec_t cfg) {
  // start with av_render_init
  xSemaphore_subscribe = xSemaphoreCreateMutex();
  if (xSemaphore_subscribe == NULL) {
    ESP_LOGE(TAG, "Failed to create subscribe mutex");
    return SUBSCRIBE_MUTEX_CREATE_FAILED;
  }
  
  int ret = audio_av_render_init();
  if (ret >= 0) {
    ESP_LOGI(TAG, "AV render initialization successful");
  } else {
    ESP_LOGE(TAG, "AV render initialization failed");
    return SUBSCRIBE_AV_RENDER_FAILED;
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
   if (!g_pc_subscribe) {
     ESP_LOGE(TAG, "Failed to create subscribe peer connection");
     return SUBSCRIBE_PEER_CONNECTION_FAILED;
   }
   
   peer_connection_oniceconnectionstatechange(g_pc_subscribe, oniceconnectionstatechangeSubscribe);
 ServiceConfiguration service_config = SERVICE_CONFIG_DEFAULT(); 
  service_config.pc = g_pc_subscribe;
  // service_config.hostname = "dev-whip.videosdk.live";
  // service_config.path = "/whep";
  // service_config.http_url = "dev-whip.videosdk.live";
  service_config.hostname = "dev-api.videosdk.live";
  service_config.path = "/v2/whep?roomId=roye-pqdd-wbfl&participantId=whip-peer&remotePeerId=bbupzxbG";
  service_config.http_url = "dev-api.videosdk.live";
  service_config.http_port = 443;
  service_config.auth_token = g_token_videosdk;
  // set service config
  printf("Setting service configuration: %s\n", service_config.auth_token);
   peer_signaling_set_config(&service_config);
// whep connect request for create offer
  if (peer_signaling_whep_connect() != 0) {
    ESP_LOGE(TAG, "WHEP connection failed");
    return SUBSCRIBE_WHEP_CONNECT_FAILED;
  }  
  peer_connection_task_t* task_args = malloc(sizeof(peer_connection_task_t));
  if (!task_args) {
    ESP_LOGE(TAG, "Failed to allocate memory for subscribe task");
    return SUBSCRIBE_MEMORY_ALLOC_FAILED;
  }

  task_args->pc = g_pc_subscribe;
  
  xPcTaskHandleSubscribe = xTaskCreatePinnedToCore(peer_connection_task_subscribe, "peer_connection", 8192, task_args, 5, &xPcTaskHandleSubscribe, 1);
  if (xPcTaskHandleSubscribe == NULL) {
    ESP_LOGE(TAG, "Failed to create subscribe task");
    free(task_args);
    return SUBSCRIBE_TASK_CREATE_FAILED;
  }

    
    return RESULT_OK;
}




// int stopSubscribeAudio(){
// audio_deinit();

// }

result_t startPublishAudio(audio_codec_t cfg) {
  printf("Inside the startPublish Function\n");

  if (xSemaphore_publish == NULL) {
   xSemaphore_publish = xSemaphoreCreateMutex();
    if (xSemaphore_publish == NULL) {
      ESP_LOGE(TAG, "Failed to create mutex");
      return PUBLISH_MUTEX_CREATE_FAILED;
    }
  }

  int ret = audio_codec_init(cfg);
  if (ret >= 0) {
    ESP_LOGI(TAG, "Audio codec initialization successful");
  } else {
    ESP_LOGE(TAG, "Audio codec initialization failed");
    return PUBLISH_AUDIO_CODEC_FAILED;
  }

  PeerConfiguration config = {
      .ice_servers = {
          {.urls = "stun:stun.l.google.com:19302"}},
      .audio_codec = CODEC_OPUS,
  };

  g_pc_publish = peer_connection_create(&config, true, false);
  if (!g_pc_publish) {
    ESP_LOGE(TAG, "Failed to create peer connection");
    return PUBLISH_PEER_CONNECTION_FAILED;
  }

  peer_connection_oniceconnectionstatechange(g_pc_publish, oniceconnectionstatechangePublish);

  ServiceConfiguration service_config = SERVICE_CONFIG_DEFAULT();
  service_config.pc = g_pc_publish;
  service_config.hostname = "dev-api.videosdk.live";
  service_config.path = "/v2/whip?roomId=roye-pqdd-wbfl&participantId=whip-peer&displayName=whip-participant";
  service_config.http_url = "dev-api.videosdk.live";
  service_config.http_port = 443;
  service_config.auth_token = g_token_videosdk;
  
  printf("Setting service configuration: %s\n", service_config.auth_token);
  peer_signaling_set_config(&service_config);
  
  // whip connect request for create offer
  if (peer_signaling_whip_connect() != 0) {
    ESP_LOGE(TAG, "WHIP connection failed");
    return PUBLISH_WHIP_CONNECT_FAILED;
  }

  StaticTask_t *audio_task_buffer = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
  StackType_t *audio_stack = heap_caps_malloc(20480 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
  if (audio_task_buffer && audio_stack) {
    xAudioTaskHandle = xTaskCreateStaticPinnedToCore(audio_task, "audio", 20480, NULL, 9, audio_stack, audio_task_buffer, 0);
    if (xAudioTaskHandle == NULL) {
      ESP_LOGE(TAG, "Failed to create audio task");
      return PUBLISH_TASK_CREATE_FAILED;
    }
  } else {
    ESP_LOGE(TAG, "Failed to allocate memory for audio task");
    return PUBLISH_MEMORY_ALLOC_FAILED;
  }

  StaticTask_t *pc_task_buffer = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
  StackType_t *pc_stack = heap_caps_malloc(16384 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
  peer_connection_task_t *task_args = heap_caps_malloc(sizeof(peer_connection_task_t), MALLOC_CAP_INTERNAL);
  if (pc_task_buffer && pc_stack && task_args) {
    task_args->pc = g_pc_publish;
    xPcTaskHandlePublish = xTaskCreateStaticPinnedToCore(peer_connection_task_publish, "peer_connection", 16384, NULL, 5, pc_stack, pc_task_buffer, 1);
    if (xPcTaskHandlePublish == NULL) {
      ESP_LOGE(TAG, "Failed to create peer connection task");
      return PUBLISH_TASK_CREATE_FAILED;
    }
  } else {
    ESP_LOGE(TAG, "Failed to allocate memory for peer_connection task");
    return PUBLISH_MEMORY_ALLOC_FAILED;
  }

  // ✅ Wait until peer connection completes using polling (could be replaced with event/semaphore)
  while (eState != PEER_CONNECTION_COMPLETED) {
   printf("Waiting for peer connection to complete...\n");
   // esp_task_wdt_reset(); // Yield to avoid WDT trigger
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  printf("Peer connection completed successfully\n");

  return RESULT_OK;
}
void stop_publish_task(void *param) {
  ESP_LOGI(TAG, "Stopping audio and peer...");

  // ✅ Suspend audio task
  if (xAudioTaskHandle != NULL) {
    vTaskSuspend(xAudioTaskHandle);
    ESP_LOGI(TAG, "Audio task suspended");
  }


  delete_peer_from_meeting(); 

  // ✅ Suspend peer connection task
  if (xPcTaskHandlePublish != NULL) {
    vTaskSuspend(xPcTaskHandlePublish);
    ESP_LOGI(TAG, "Peer connection task suspended");
  }



  ESP_LOGI(TAG, "Publishing stopped");

  vTaskDelete(NULL);  
}
result_t stopPublishAudio() {
  if (xTaskCreatePinnedToCore(stop_publish_task, "stop_publish_task", 8192, NULL, 5, NULL, 0) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create stop publish task");
    return STOP_PUBLISH_TASK_CREATE_FAILED;
  }
  return RESULT_OK;
}
