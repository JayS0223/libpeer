#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_enc_reg.h"
#include <string.h>
#include "peer_connection.c"
#include "peer.h"
#include "sdkconfig.h"
#include "driver/i2s_pdm.h"
#include <math.h> 
#include "audio_decoder.h"
#include "av_render.h"
#include "av_render_default.h"
#include "codec_board.h"
#include "esp_audio_dec_default.h"
#include "codec_init.h"

typedef struct {
    audio_render_handle_t audio_render;
    av_render_handle_t    player;
} player_system_t;


static player_system_t  player_sys;

#if defined(CONFIG_ESP32S3_XIAO)
#define I2S_CLK_GPIO 42
#define I2S_DATA_GPIO 41
#endif

#if defined(CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD)
#define I2S_CLK_GPIO 4     // BCLK
#define I2S_WS_GPIO  6     // WS / LRCK
#define I2S_DOUT_GPIO 5    
#endif


static bool subscribed = false;
#define TAG "AUDIO"
// === Audio Playback Config ===
#define AUDIO_FRAME_MAX_SIZE 640   // 20ms PCM16 mono @ 8kHz = 160 samples * 2 bytes
#define AUDIO_QUEUE_LEN 10

typedef struct {
    uint8_t data[AUDIO_FRAME_MAX_SIZE];
    size_t length;
} AudioFrame_t;


static i2s_chan_handle_t tx_handle = NULL;  // I2S TX for playback
static QueueHandle_t audio_queue = NULL;

#define FRAME_MS         20                      // 20 ms audio frame
#define SAMPLE_RATE      8000                   // 16 kHz audio
#define SAMPLE_SIZE      2                       // 16-bit PCM = 2 bytes
#define CHANNELS         1                     // Mono
#define FRAME_LEN        (SAMPLE_RATE / 1000 * FRAME_MS * SAMPLE_SIZE * CHANNELS) // 640 bytes
int64_t last_patch_time = 0;
extern PeerConnection* g_pc;
extern PeerConnectionState eState;
extern int get_timestamp();
int64_t curr_time = 0;
static esp_codec_dev_handle_t record_handle = NULL;
static esp_audio_enc_handle_t enc_handle = NULL;
static esp_audio_enc_in_frame_t aenc_in_frame = {0};
static esp_audio_enc_out_frame_t aenc_out_frame = {0};
esp_opus_enc_config_t opus_enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
esp_g711_enc_config_t g711_cfg;
esp_audio_enc_config_t enc_cfg;
i2s_chan_handle_t rx_handle = NULL;


int32_t audio_get_samples(uint8_t* buf, size_t size);

static uint8_t* read_buf = NULL;
static uint8_t* write_buf = NULL;


esp_err_t audio_codec_init() {

#if CONFIG_ESP32S3_XIAO
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

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
#endif

 esp_audio_enc_register_default();
 
    int read_size = 0, out_size = 0;
    g711_cfg.sample_rate = 8000;
    g711_cfg.channel = 1;
    g711_cfg.bits_per_sample = 16;
    g711_cfg.frame_duration = 20;

    // opus_enc_cfg.sample_rate = 16000;
    // opus_enc_cfg.channel = 2;
    // opus_enc_cfg.bits_per_sample = 16;
    // opus_enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    // opus_enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;


    enc_cfg.type = ESP_AUDIO_TYPE_G711A;
    enc_cfg.cfg = &g711_cfg;
    enc_cfg.cfg_sz = sizeof(g711_cfg);

#if CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD
   
    record_handle = get_record_handle();
    if (!record_handle) {
        ESP_LOGE(TAG, "Failed to get record handle");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Record handle initialized: %p", record_handle);

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 8000,
        .channel = 1,
        .bits_per_sample = 16,
    };
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
    printf("Read buffer : %p",read_buf);
    aenc_in_frame.len = read_size;
    aenc_out_frame.buffer = write_buf;
    aenc_out_frame.len = out_size;

    ESP_LOGI(TAG, "Audio codec init done. Read size: %d, Out size: %d", read_size, out_size);
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
#if CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD
    int32_t audio_get_samples(uint8_t* buf, size_t size) {
        printf("Lol I am inside get audio samples\n");
        printf("record handle %p \n",record_handle);
        printf("size %d \n",size);
        // printf("buffer %hhn\n",buf);
        esp_err_t err = esp_codec_dev_read(record_handle, (char*)buf, size);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Codec read error: %d", err);
            return -1;
        }
        return size;
    }
#elif CONFIG_ESP32S3_XIAO
int32_t audio_get_samples(uint8_t* buf, size_t size) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_handle, (char*)buf, size, &bytes_read, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s read error: %d", err);
    }
    return bytes_read;

}
#endif

void audio_task(void* arg) {
    int ret;
    static int64_t last_time, last_log_time;
    int64_t curr_time;
    float bytes = 0;

    last_time = get_timestamp();
    last_log_time = last_time;
    ESP_LOGI(TAG, "audio task started");

    for (;;) {
        if (eState == PEER_CONNECTION_COMPLETED) {
            // 💡 Add a yield at the start to reset WDT even if blocked before
            taskYIELD();  // or esp_task_wdt_reset();

            ret = audio_get_samples(aenc_in_frame.buffer, aenc_in_frame.len);
            if (ret == aenc_in_frame.len) {
                // Optional yield between major steps
                taskYIELD();

                esp_audio_err_t enc_ret = esp_audio_enc_process(enc_handle, &aenc_in_frame, &aenc_out_frame);
                if (enc_ret == ESP_AUDIO_ERR_OK) {
                    int send_ret = peer_connection_send_audio(g_pc, aenc_out_frame.buffer, aenc_out_frame.encoded_bytes);
                    if (send_ret < 0) {
                        ESP_LOGW(TAG, "peer_connection_send_audio failed: %d", send_ret);
                    } else {
                        ESP_LOGI(TAG, "Sent audio: %d bytes", aenc_out_frame.encoded_bytes);
                    }
                    bytes += aenc_out_frame.encoded_bytes;
                } else {
                    ESP_LOGE(TAG, "Audio encode failed: %d", enc_ret);
                }
            } else {
                ESP_LOGW(TAG, "Partial audio frame: %d/%d bytes", ret, aenc_in_frame.len);
            }

            curr_time = get_timestamp();
            if ((curr_time - last_log_time) > 5000) {
                float bitrate = 1000.0 * (bytes * 8.0 / (curr_time - last_time));
                ESP_LOGI(TAG, "audio bitrate: %.1f bps | free heap: %d | stack watermark: %d",
                         bitrate, esp_get_free_heap_size(), uxTaskGetStackHighWaterMark(NULL));
                last_time = curr_time;
                last_log_time = curr_time;
                bytes = 0;
            }

            // 💡 Keep a small delay to avoid hogging CPU
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            ESP_LOGD(TAG, "PeerConnection not ready (state=%d), skipping audio send", eState);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
