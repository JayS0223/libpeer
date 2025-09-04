#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "peer.h"
#include "board.c"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "codec_board.h"
#include "videosdk.h"


static const char* TAG = "webrtc";

// static TaskHandle_t xPcTaskHandle = NULL;
// static TaskHandle_t xCameraTaskHandle = NULL;
// static TaskHandle_t xAudioTaskHandle = NULL;

extern esp_err_t camera_init();
extern esp_err_t audio_init();
extern void camera_task(void* pvParameters);
extern void audio_task(void* pvParameters);
extern esp_err_t audio_codec_init();
extern int init_board();
//extern void audio_receive_g711a_and_render(const uint8_t* encoded_data, size_t encoded_len, uint32_t timestamp);
extern void audio_decode_init();
extern void audio_av_render_init();
extern void audio_receive_and_render(const uint8_t* encoded_data, size_t encoded_len, uint32_t timestamp);
extern void removePeer();
 const char *token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJhcGlrZXkiOiI0N2M3ZTJlYy01NzY5LTQ3OWQtYjdjNS0zYjU5MDcxYzhhMDkiLCJwZXJtaXNzaW9ucyI6WyJhbGxvd19qb2luIl0sImlhdCI6MTY3MjgwOTcxMywiZXhwIjoxODMwNTk3NzEzfQ.KeXr1cxORdq6X7-sxBLLV7MsUnwuJGLaG8_VTyTFBig";
extern void loop_log();
// SemaphoreHandle_t xSemaphore = NULL;

// PeerConnection* g_pc;
// PeerConnectionState eState = PEER_CONNECTION_CLOSED;
int gDataChannelOpened = 0;

// int64_t get_timestamp() {
//   struct timeval tv;
//   gettimeofday(&tv, NULL);
//   return (tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL));
// }

// static void oniceconnectionstatechange(PeerConnectionState state, void* user_data) {
//   ESP_LOGI(TAG, "PeerConnectionState: %d", state);
//   eState = state;
//   // not support datachannel close event
//   if (eState != PEER_CONNECTION_COMPLETED) {
//     gDataChannelOpened = 0;
//   }
// }

static void thread_scheduler(const char *thread_name, media_lib_thread_cfg_t *thread_cfg)
{
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


// static void oniceconnectionstatechange(PeerConnectionState state, void* user_data) {
//   ESP_LOGI(TAG, "PeerConnectionState changed: %d (%s)", state, peer_connection_state_to_string(state));
//   eState = state;

//   // if (on_connection_state_changed_cb) {
//   //   on_connection_state_changed_cb(state);  // Invoke the user-defined callback
//   // }

//   switch (state) {
//     case PEER_CONNECTION_CONNECTED:
//       ESP_LOGI(TAG, "DTLS handshake completed, connection is now CONNECTED");
//       break;
//     case PEER_CONNECTION_COMPLETED:
//       ESP_LOGI(TAG, "ICE and DTLS completed, connection is now COMPLETED");
//       break;
//     case PEER_CONNECTION_FAILED:
//       ESP_LOGE(TAG, "PeerConnection FAILED");
//       break;
//     case PEER_CONNECTION_CLOSED:
//       ESP_LOGW(TAG, "PeerConnection CLOSED");
//       break;
//     default:
//       break;
//   }
// }
static void onmessage(char* msg, size_t len, void* userdata, uint16_t sid) {
  ESP_LOGI(TAG, "Datachannel message: %.*s", len, msg);
}

void onopen(void* userdata) {
  ESP_LOGI(TAG, "Datachannel opened");
  gDataChannelOpened = 1;
}

static void onclose(void* userdata) {
}

// void peer_connection_task(void* arg) {
//   ESP_LOGI(TAG, "peer_connection_task started");
//   connection_config_t* config = (connection_config_t*) arg; 

//   for (;;) {
//     if (xSemaphoreTake(xSemaphore, portMAX_DELAY)) {
//       peer_connection_loop(g_pc);
//       xSemaphoreGive(xSemaphore);
//     }

//     vTaskDelay(pdMS_TO_TICKS(1));
//   }
// }
// static void meeting_task(void *pvParameters)
// {
//     const char *auth_token = (const char *)pvParameters; 
//     create_meeting_config_t createMeetingConfig = {
//       .token = token,
//       .customMeetingId = "jay-shah"
//     };
//     ESP_LOGI(TAG, "meeting_task started");

//     char *room_id = create_meeting(&createMeetingConfig);
//     if (room_id) {
//         ESP_LOGI(TAG, "Created meeting roomId = %s", room_id);
//         free(room_id);
//     } else {
//         ESP_LOGE(TAG, "Failed to create meeting");
//     }

//     ESP_LOGI(TAG, "meeting_task finished, deleting self");
//     vTaskDelete(NULL);
// }


void app_main(void) {
  static char deviceid[32] = {0};
  uint8_t mac[8] = {0};

  ESP_LOGI(TAG, "[APP] Startup..");
  ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
  esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
  esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(example_connect());

  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
    sprintf(deviceid, "esp32-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device ID: %s", deviceid);
  }

  // xSemaphore = xSemaphoreCreateMutex();
  media_lib_add_default_adapter(); 
 
  media_lib_thread_set_schedule_cb(thread_scheduler);
// char *meeting_id = create_meeting("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJhcGlrZXkiOiI0N2M3ZTJlYy01NzY5LTQ3OWQtYjdjNS0zYjU5MDcxYzhhMDkiLCJwZXJtaXNzaW9ucyI6WyJhbGxvd19qb2luIl0sImlhdCI6MTY3MjgwOTcxMywiZXhwIjoxODMwNTk3NzEzfQ.KeXr1cxORdq6X7-sxBLLV7MsUnwuJGLaG8_VTyTFBig");
// BaseType_t ok = xTaskCreate(meeting_task, "meeting_task", 16384, (void *)token, 5, NULL);
//   if (ok != pdPASS) {
//       ESP_LOGE(TAG, "Failed to create meeting_task");
//   }
init_config_t init_cfg = {
    .meetingID = "roye-pqdd-wbfl",
    .token = token,
    .displayName = "ESP32 Device",
    .participantId = NULL,
    
  };
  audio_codec_t cfg_publish = AUDIO_CODEC_OPUS;
  audio_codec_t subscribe = AUDIO_CODEC_OPUS;
  init(&init_cfg);
  //  subs_cfg = {
  //   .codec = subscribe,
  //   .peerId = "qer46y57ui",
  // };
  startPublishAudio(cfg_publish);
   // vTaskDelay(pdMS_TO_TICKS(15000));
  startSubscribeAudio(subscribe);
  

 

  while (1) {
    //printf("Waiting for task to complete from main\n");
    // peer_signaling_loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
