#include "videosdk.h"
#include "peer.h"
#include "esp_log.h"
#include "videosdk_audio.h"
#include <stdbool.h> 
static const char* TAG = "videosdk";
char* local_meetingID = NULL;
char* local_token = NULL;
bool local_enableMic = true;
static PeerConnection* g_pc = NULL;
static PeerConnectionState eState = PEER_CONNECTION_CLOSED;

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
  }

esp_err_t videosdk_init(const char* meetingId, const char* token, const bool enableMic, const char* displayName) {

local_meetingID = meetingId;
local_token = token;
local_enableMic = enableMic;
  ESP_LOGI(TAG, "Initializing Video SDK with meetingId: %s, token: %s, enableMic: %d, displayName: %s",
           local_meetingID, local_token, local_enableMic, displayName);
  // Initialize audio if microphone is enabled

  PeerConfiguration config = {
    .ice_servers = {
        {.urls = "stun:stun.l.google.com:19302"},
    },
    #if(local_enableMic)
    .audio_codec = CODEC_PCMA,
    #endif
  };
peer_init();
  ESP_LOGI(TAG, "Initializing Video SDK...");

  g_pc = peer_connection_create(&config);
  if (!g_pc) {
    ESP_LOGE(TAG, "Failed to create PeerConnection!");
    return ESP_FAIL;
  }
ESP_LOGI(TAG, "PeerConnection created: %p", g_pc);
peer_connection_oniceconnectionstatechange(g_pc, oniceconnectionstatechange);

  ESP_LOGI(TAG, "Video SDK initialized successfully");
  return ESP_OK;
}

esp_err_t videosdk_connect() {

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
  
  peer_signaling_set_config(&service_config);
  ESP_LOGI(TAG, "Connecting to signaling server...");
  peer_signaling_whip_connect();
  ESP_LOGI(TAG, "Peer signaling configuration set (WHIP)");
  return ESP_OK;
}

void videosdk_loop() {
  if (!g_pc) {
    ESP_LOGE(TAG, "PeerConnection is not initialized");
    return;
  }
  // Run the peer connection loop
  peer_connection_loop(g_pc);
}
void videosdk_publish(){
  ESP_LOGI(TAG, "Audio init  stream...");
  videosdk_audio_init();
  ESP_LOGI(TAG, "Audio initialized");
}

void videosdk_subscribe(){

}

void videosdk_disconnect(){

}