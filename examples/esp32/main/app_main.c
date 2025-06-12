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
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "peer.h"
#include "videosdk.h"

static const char* TAG = "webrtc";

static TaskHandle_t xPcTaskHandle = NULL;
static TaskHandle_t xCameraTaskHandle = NULL;
static TaskHandle_t xAudioTaskHandle = NULL;

extern esp_err_t camera_init();
extern esp_err_t audio_init();
extern void camera_task(void* pvParameters);
extern void audio_task(void* pvParameters);

SemaphoreHandle_t xSemaphore = NULL;

PeerConnection* g_pc;
PeerConnectionState eState = PEER_CONNECTION_CLOSED;
int gDataChannelOpened = 0;

int64_t get_timestamp() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL));
}

static void oniceconnectionstatechange(PeerConnectionState state, void* user_data) {
  ESP_LOGI(TAG, "PeerConnectionState changed: %d (%s)", state, peer_connection_state_to_string(state));
  eState = state;
  if (eState == PEER_CONNECTION_CONNECTED) {
    ESP_LOGI(TAG, "DTLS handshake completed, connection is now CONNECTED");
  } else if (eState == PEER_CONNECTION_COMPLETED) {
    ESP_LOGI(TAG, "ICE and DTLS completed, connection is now COMPLETED");
  } else if (eState == PEER_CONNECTION_FAILED) {
    ESP_LOGE(TAG, "PeerConnection FAILED");
  } else if (eState == PEER_CONNECTION_CLOSED) {
    ESP_LOGW(TAG, "PeerConnection CLOSED");
  }
  // not support datachannel close event
  if (eState != PEER_CONNECTION_COMPLETED) {
    gDataChannelOpened = 0;
  }
}

static void onmessage(char* msg, size_t len, void* userdata, uint16_t sid) {
  ESP_LOGI(TAG, "Datachannel message: %.*s", len, msg);
}

void onopen(void* userdata) {
  ESP_LOGI(TAG, "Datachannel opened");
  gDataChannelOpened = 1;
}

static void onclose(void* userdata) {
  ESP_LOGI(TAG, "Datachannel closed");
  gDataChannelOpened = 0;
}

void peer_connection_task(void* arg) {
  ESP_LOGI(TAG, "peer_connection_task started");

  for (;;) {
    if (xSemaphoreTake(xSemaphore, portMAX_DELAY)) {
      ESP_LOGD(TAG, "Calling peer_connection_loop, current state: %d (%s)", eState, peer_connection_state_to_string(eState));
      peer_connection_loop(g_pc);
      ESP_LOGI(TAG, "PeerConnection state after loop: %d (%s)", eState, peer_connection_state_to_string(eState));
      xSemaphoreGive(xSemaphore);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void app_main(void) {
  static char deviceid[32] = {0};
  uint8_t mac[8] = {0};

  PeerConfiguration config = {
    .ice_servers = {
        {.urls = "stun:stun.l.google.com:19302"},
    },
#if defined(CONFIG_WHIP_URL)
   // .video_codec = CODEC_H264,
   .audio_codec = CODEC_OPUS
#else
    // .audio_codec = CODEC_OPUS,
    .datachannel = DATA_CHANNEL_BINARY,
#endif
  };

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

  xSemaphore = xSemaphoreCreateMutex();

  peer_init();

  camera_init();

#if defined(CONFIG_ESP32S3_XIAO_SENSE)
  audio_init();
#endif

  ESP_LOGI(TAG, "Creating PeerConnection...");
  g_pc = peer_connection_create(&config);
  if (!g_pc) {
    ESP_LOGE(TAG, "Failed to create PeerConnection!");
    return;
  }
  ESP_LOGI(TAG, "PeerConnection created: %p", g_pc);
  peer_connection_oniceconnectionstatechange(g_pc, oniceconnectionstatechange);
  peer_connection_ondatachannel(g_pc, onmessage, onopen, onclose);

  ServiceConfiguration service_config = SERVICE_CONFIG_DEFAULT();
  service_config.pc = g_pc;

#if defined(CONFIG_WHIP_URL)
  service_config.http_url = CONFIG_WHIP_URL;
  service_config.http_port = CONFIG_WHIP_PORT;
 // service_config.bearer_token = CONFIG_WHIP_BEARER_TOKEN;
#else
  service_config.client_id = deviceid;
  service_config.mqtt_url = "broker.emqx.io";
#endif
  
  ESP_LOGI(TAG, "Setting up signaling...");
  peer_signaling_set_config(&service_config);


#if defined(CONFIG_WHIP_URL)
  peer_signaling_whip_connect();
  ESP_LOGI(TAG, "Peer signaling configuration set (WHIP)");
#else
  peer_signaling_join_channel();
  ESP_LOGI(TAG, "Peer signaling configuration set (MQTT)");
#endif

#if defined(CONFIG_ESP32S3_XIAO_SENSE)
  StackType_t* stack_memory = (StackType_t*)heap_caps_malloc(8192 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
  StaticTask_t task_buffer;
  if (stack_memory) {
    xAudioTaskHandle = xTaskCreateStaticPinnedToCore(audio_task, "audio", 8192, NULL, 7, stack_memory, &task_buffer, 0);
  }
#endif

  xTaskCreatePinnedToCore(camera_task, "camera", 4096, NULL, 8, &xCameraTaskHandle, 1);

  xTaskCreatePinnedToCore(peer_connection_task, "peer_connection", 8192, NULL, 5, &xPcTaskHandle, 1);

  ESP_LOGI(TAG, "Starting camera and peer connection tasks...");

  ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "open https://sepfy.github.io/webrtc?deviceId=%s", deviceid);

  while (1) {
    peer_signaling_loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
