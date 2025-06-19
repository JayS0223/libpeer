#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_enc_reg.h"
#include "esp_g711_enc.h"

#include "peer_connection.h"

#define I2S_CLK_GPIO 42
#define I2S_DATA_GPIO 41

static const char* TAG = "AUDIO";

extern PeerConnection* g_pc;
extern PeerConnectionState eState;
extern int get_timestamp();

i2s_chan_handle_t rx_handle = NULL;
esp_audio_enc_handle_t enc_handle = NULL;
esp_audio_enc_in_frame_t aenc_in_frame = {0};
esp_audio_enc_out_frame_t aenc_out_frame = {0};
esp_g711_enc_config_t g711_cfg;
esp_audio_enc_config_t enc_cfg;

static uint8_t* read_buf = NULL;
static uint8_t* write_buf = NULL;

esp_err_t audio_codec_init() {
    int read_size = 0, out_size = 0;

    esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
    esp_audio_enc_register_default();

    g711_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_8K;
    g711_cfg.channel = ESP_AUDIO_MONO;
    g711_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    g711_cfg.frame_duration = 10;

    enc_cfg.type = ESP_AUDIO_TYPE_G711A;
    enc_cfg.cfg = &g711_cfg;
    enc_cfg.cfg_sz = sizeof(g711_cfg);

    ESP_LOGI(TAG, "Initializing encoder: G711A");
    ESP_LOGI(TAG, "Encoder config: %d Hz, %d ch, %d bits, %d ms frame",
             g711_cfg.sample_rate, g711_cfg.channel, g711_cfg.bits_per_sample, g711_cfg.frame_duration);
    ESP_LOGI(TAG, "Free heap before encoder open: %d", esp_get_free_heap_size());

    ret = esp_audio_enc_open(&enc_cfg, &enc_handle);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Encoder open failed: %d", ret);
        return ESP_FAIL;
    }

    esp_audio_enc_get_frame_size(enc_handle, &read_size, &out_size);

    read_buf = malloc(read_size);
    write_buf = malloc(out_size);
    if (!read_buf || !write_buf) {
        ESP_LOGE(TAG, "Encoder buffer malloc failed");
        return ESP_FAIL;
    }

    aenc_in_frame.buffer = read_buf;
    aenc_in_frame.len = read_size;
    aenc_out_frame.buffer = write_buf;
    aenc_out_frame.len = out_size;

    ESP_LOGI(TAG, "Audio codec init done. Read size: %d, Out size: %d", read_size, out_size);
    return ESP_OK;
}

esp_err_t audio_init(void) {
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

    return audio_codec_init();
}

void audio_deinit(void) {
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
    }
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

int32_t audio_get_samples(uint8_t* buf, size_t size) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_handle, (char*)buf, size, &bytes_read, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s read error: %d", err);
    }
    return bytes_read;
}

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
