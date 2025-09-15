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
#include "freertos/FreeRTOS.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "videosdk.h"

static const char* TAG = "webrtc";

const char* token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJhcGlrZXkiOiI1MzE0YzVkZC0wN2MzLTRjZTgtYThmYi03ZmY0ZDZiMDdhYTIiLCJwZXJtaXNzaW9ucyI6WyJhbGxvd19qb2luIl0sImlhdCI6MTc1MTI2MTQ4MCwiZXhwIjoxNzgyNzk3NDgwfQ.yMH2talasjiL7ftJt2hl9h6_L96G1YU_tjujycAEi9M";

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

  // char *meeting_id = create_meeting("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJhcGlrZXkiOiI0N2M3ZTJlYy01NzY5LTQ3OWQtYjdjNS0zYjU5MDcxYzhhMDkiLCJwZXJtaXNzaW9ucyI6WyJhbGxvd19qb2luIl0sImlhdCI6MTY3MjgwOTcxMywiZXhwIjoxODMwNTk3NzEzfQ.KeXr1cxORdq6X7-sxBLLV7MsUnwuJGLaG8_VTyTFBig");
  // BaseType_t ok = xTaskCreate(meeting_task, "meeting_task", 16384, (void *)token, 5, NULL);
  //   if (ok != pdPASS) {
  //       ESP_LOGE(TAG, "Failed to create meeting_task");
  //   }
  init_config_t init_cfg = {
      .meetingID = "fu4f-wh7j-kf01",
      .token = token,
      .displayName = "ESP32-Device",
      .audioCodec = AUDIO_CODEC_OPUS,
  };

  result_t init_result = init(&init_cfg);
  printf("Result: %d\n", init_result);
  result_t result_publish = startPublishAudio(NULL);
  result_t result_susbcribe = startSubscribeAudio(NULL, "tTNlWEZK");
  vTaskDelay(pdMS_TO_TICKS(100000));
  result_t result_leave = leave();
  printf("Result:%d\n", result_publish);
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
