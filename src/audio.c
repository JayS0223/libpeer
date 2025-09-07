#include <math.h>

#include <string.h>
#include "audio_decoder.h"
#include "av_render.h"
#include "av_render_default.h"
#include "codec_board.h"
#include "codec_init.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_enc_reg.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "peer.h"
#include "peer_connection.c"
#include "peer_signaling.h"
#include "sdkconfig.h"
#include "videosdk.c"
#include "videosdk.h"
#define CUSTOM_HEADER_SIZE 16
typedef struct {
  audio_render_handle_t audio_render;
  av_render_handle_t player;
} player_system_t;

extern TaskHandle_t xSubscribeAudioTaskHandle;
static player_system_t player_sys;

#if defined(CONFIG_ESP32S3_XIAO)
#define I2S_CLK_GPIO 42
#define I2S_DATA_GPIO 41
#endif

#if defined(CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD)
#define I2S_CLK_GPIO 4  // BCLK
#define I2S_WS_GPIO 6   // WS / LRCK
#define I2S_DOUT_GPIO 5
#endif

static bool subscribed = false;
#define TAG "AUDIO"
// === Audio Playback Config ===
#define AUDIO_FRAME_MAX_SIZE 640  // 20ms PCM16 mono @ 8kHz = 160 samples * 2 bytes
#define AUDIO_QUEUE_LEN 10

typedef struct {
  uint8_t data[AUDIO_FRAME_MAX_SIZE];
  size_t length;
} AudioFrame_t;

static i2s_chan_handle_t tx_handle = NULL;  // I2S TX for playback
static QueueHandle_t audio_queue = NULL;

#define FRAME_MS 20                                                         // 20 ms audio frame
#define SAMPLE_RATE 8000                                                    // 16 kHz audio
#define SAMPLE_SIZE 2                                                       // 16-bit PCM = 2 bytes
#define CHANNELS 1                                                          // Mono
#define FRAME_LEN (SAMPLE_RATE / 1000 * FRAME_MS * SAMPLE_SIZE * CHANNELS)  // 640 bytes
int64_t last_patch_time = 0;
extern PeerConnection* g_pc_publish;
extern PeerConnectionState eState;
int64_t get_timestamp() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL));
}
int64_t curr_time = 0;
static esp_codec_dev_handle_t record_handle = NULL;
static esp_audio_enc_handle_t enc_handle = NULL;
static esp_audio_enc_in_frame_t aenc_in_frame = {0};
static esp_audio_enc_out_frame_t aenc_out_frame = {0};
esp_opus_enc_config_t opus_enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
esp_g711_enc_config_t g711_cfg;
esp_audio_enc_config_t enc_cfg;
i2s_chan_handle_t rx_handle = NULL;
av_render_audio_info_t audio_info;
esp_codec_dev_sample_info_t fs;
av_render_audio_frame_info_t aud_info;
// int32_t audio_get_samples(uint8_t* buf, size_t size);

static uint8_t* read_buf = NULL;
static uint8_t* write_buf = NULL;

esp_err_t audio_codec_init(audio_codec_t cfg) {
#if CONFIG_ESP32S3_XIAO

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

  if (cfg == AUDIO_CODEC_OPUS) {
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = I2S_CLK_GPIO,
            .din = I2S_DATA_GPIO,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
  } else {
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(8000),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = I2S_CLK_GPIO,
            .din = I2S_DATA_GPIO,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
  }

#endif

  esp_audio_enc_register_default();

  int read_size = 0, out_size = 0;
  if (cfg == AUDIO_CODEC_OPUS) {
    opus_enc_cfg.sample_rate = 16000;
    opus_enc_cfg.channel = 1;
    opus_enc_cfg.bits_per_sample = 16;
    opus_enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    opus_enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;

    enc_cfg.type = ESP_AUDIO_TYPE_OPUS;
    enc_cfg.cfg = &opus_enc_cfg;
    enc_cfg.cfg_sz = sizeof(opus_enc_cfg);

  } else if (cfg == AUDIO_CODEC_G711U) {
    g711_cfg.sample_rate = 8000;
    g711_cfg.channel = 1;
    g711_cfg.bits_per_sample = 16;
    g711_cfg.frame_duration = 20;
    enc_cfg.type = ESP_AUDIO_TYPE_G711U;
    enc_cfg.cfg = &g711_cfg;
    enc_cfg.cfg_sz = sizeof(g711_cfg);
  } else {
    g711_cfg.sample_rate = 8000;
    g711_cfg.channel = 1;
    g711_cfg.bits_per_sample = 16;
    g711_cfg.frame_duration = 20;
    enc_cfg.type = ESP_AUDIO_TYPE_G711A;
    enc_cfg.cfg = &g711_cfg;
    enc_cfg.cfg_sz = sizeof(g711_cfg);
  }

#if CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD

  record_handle = get_record_handle();
  if (!record_handle) {
    ESP_LOGE(TAG, "Failed to get record handle");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Record handle initialized: %p", record_handle);
  if (cfg == AUDIO_CODEC_OPUS) {
    fs.sample_rate = 16000;
    fs.channel = 1;
    fs.bits_per_sample = 16;

  } else {
    fs.sample_rate = 8000;
    fs.channel = 1;
    fs.bits_per_sample = 16;
  }
  esp_codec_dev_open(record_handle, &fs);

#endif
  esp_audio_err_t enc_err = esp_audio_enc_open(&enc_cfg, &enc_handle);
  if (enc_err != ESP_AUDIO_ERR_OK || !enc_handle) {
    ESP_LOGE(TAG, "Failed to open audio encoder: %d", enc_err);
    return ESP_FAIL;
  }

  // Get buffer sizes from encoder
  esp_audio_enc_get_frame_size(enc_handle, &read_size, &out_size);

  read_buf = malloc(read_size);
  write_buf = malloc(out_size);
  if (!read_buf || !write_buf) {
    ESP_LOGE(TAG, "Failed to allocate encoder buffers");
    return ESP_FAIL;
  }

  aenc_in_frame.buffer = read_buf;
  aenc_in_frame.len = read_size;
  aenc_out_frame.buffer = write_buf;
  aenc_out_frame.len = out_size;

  //  ESP_LOGI(TAG, "Audio codec init done. Read size: %d, Out size: %d", read_size, out_size);
  return ESP_OK;
}

esp_err_t audio_av_render_init(audio_codec_t codec) {
  esp_audio_dec_register_default();
  i2s_render_cfg_t i2s_cfg = {
      // videodemo .fixed_clock = true,
      .play_handle = get_playback_handle(),  // You must ensure this returns valid handle
  };

  player_sys.audio_render = av_render_alloc_i2s_render(&i2s_cfg);
  if (!player_sys.audio_render) {
    ESP_LOGE(TAG, "Failed to allocate av_render handle");
    return ESP_FAIL;
  }

  esp_codec_dev_set_out_vol(i2s_cfg.play_handle, 100);
  av_render_cfg_t render_cfg = {
      .audio_render = player_sys.audio_render,
      .audio_raw_fifo_size = 8 * 4096,
      .audio_render_fifo_size = 100 * 1024,
      .allow_drop_data = false,
  };

  player_sys.player = av_render_open(&render_cfg);
  if (codec == AUDIO_CODEC_OPUS) {
    audio_info.codec = AV_RENDER_AUDIO_CODEC_OPUS;
    audio_info.sample_rate = 16000;
    audio_info.channel = 1;
    audio_info.bits_per_sample = 16;

    aud_info.sample_rate = 16000;
    aud_info.channel = 1;
    aud_info.bits_per_sample = 16;

  } else if (codec == AUDIO_CODEC_G711A) {
    audio_info.codec = AV_RENDER_AUDIO_CODEC_G711A;
    audio_info.sample_rate = 8000;
    audio_info.channel = 1;
    audio_info.bits_per_sample = 16;

    aud_info.sample_rate = 8000;
    aud_info.channel = 1;
    aud_info.bits_per_sample = 16;

  } else {
    audio_info.codec = AV_RENDER_AUDIO_CODEC_G711U;
    audio_info.sample_rate = 8000;
    audio_info.channel = 1;
    audio_info.bits_per_sample = 16;

    aud_info.sample_rate = 8000;
    aud_info.channel = 1;
    aud_info.bits_per_sample = 16;
  }

  av_render_set_fixed_frame_info(player_sys.player, &aud_info);

  int ret = av_render_add_audio_stream(player_sys.player, &audio_info);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to add audio stream to av_render (%d)", ret);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "AV Render initialized and stream added");
  return ESP_OK;
}

void audio_deinit(void) {
  if (enc_handle) {
    esp_audio_enc_close(enc_handle);
    enc_handle = NULL;
  }
  if (read_buf) {
    free(read_buf);
    read_buf = NULL;
  }
  if (write_buf) {
    free(write_buf);
    write_buf = NULL;
  }
}

void audio_task(void* arg) {
#if CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD
  int32_t audio_get_samples(uint8_t* buf, size_t size) {
    esp_err_t err = esp_codec_dev_read(record_handle, (char*)buf, size);

    if (err != ESP_OK) {
      LOGE("Codec read error: %d", err);
      return -1;
    }
    return size;
  }

#elif CONFIG_ESP32S3_XIAO
  int32_t audio_get_samples(uint8_t* buf, size_t size) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_handle, (char*)buf, size, &bytes_read, 100);
    if (err != ESP_OK) {
      LOGE("i2s read error: %d", err);
    }
    return bytes_read;
  }
#endif
  int ret;
  static int64_t last_time, last_log_time;
  int64_t curr_time;
  float bytes = 0;

  last_time = get_timestamp();
  last_log_time = last_time;

  for (;;) {
    if (eState == PEER_CONNECTION_COMPLETED) {
      taskYIELD();
      ret = audio_get_samples(aenc_in_frame.buffer, aenc_in_frame.len);
      if (ret == aenc_in_frame.len) {
        taskYIELD();
        esp_audio_err_t enc_ret = esp_audio_enc_process(enc_handle, &aenc_in_frame, &aenc_out_frame);
        if (enc_ret == ESP_AUDIO_ERR_OK) {
          int send_ret = peer_connection_send_audio(g_pc_publish, aenc_out_frame.buffer, aenc_out_frame.encoded_bytes);
          if (send_ret < 0) {
            LOGE("FAILED TO SEND AUDIO");
          } else {
            ESP_LOGI(TAG, "Audio Sent\n");
          }
          bytes += aenc_out_frame.encoded_bytes;
        } else {
          LOGE("AUDIO ENCODE FAILED");
        }
      } else {
        LOGE("FAILED TO SEND AUDIO");
      }

      curr_time = get_timestamp();
      if ((curr_time - last_log_time) > 5000) {
        float bitrate = 1000.0 * (bytes * 8.0 / (curr_time - last_time));
        last_time = curr_time;
        last_log_time = curr_time;
        bytes = 0;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

bool remove_custom_header(const uint8_t* input_data, size_t input_len, const uint8_t** output_data, size_t* output_len) {
  // Check if payload is large enough to contain header
  if (input_len < CUSTOM_HEADER_SIZE) {
    ESP_LOGW(TAG, "Payload too small for header removal: %d bytes", (int)input_len);
    return false;
  }

  // Optional: Verify magic bytes for validation
  if (input_data[0] == 0xBE && input_data[1] == 0xDE) {
  } else {
    ESP_LOGE(TAG, "Unexpected magic bytes: %02X %02X", input_data[0], input_data[1]);
    // You might want to return false here if validation is critical
  }

  // Skip header and return pointer to actual data
  *output_data = input_data + CUSTOM_HEADER_SIZE;
  *output_len = input_len - CUSTOM_HEADER_SIZE;

  return true;
}

void audio_receive_and_render(const uint8_t* encoded_data, size_t encoded_len, uint32_t timestamp) {
  const uint8_t* audio_payload = NULL;
  size_t audio_payload_len = 0;

  const uint8_t* final_audio_data = NULL;
  size_t final_audio_len = 0;

  if (!remove_custom_header(encoded_data, encoded_len, &final_audio_data, &final_audio_len)) {
    ESP_LOGE(TAG, "Failed to remove custom header from audio payload.");
    return;
  }

  av_render_audio_data_t audio_data = {
      .pts = timestamp,
      .data = (uint8_t*)final_audio_data,
      .size = final_audio_len * sizeof(uint8_t),  // Use byte size here
  };

  int ret = av_render_add_audio_data(player_sys.player, &audio_data);
  if (ret != 0) {
    ESP_LOGE(TAG, "Audio render failed (ret=%d)", ret);
  } else {
    ESP_LOGI(TAG, "Rendered %d bytes of audio", (int)final_audio_len);
  }
}
