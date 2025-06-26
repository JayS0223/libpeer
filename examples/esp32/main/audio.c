#include "driver/i2s_std.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_enc_reg.h"
#include "esp_g711_enc.h"
#include "es8311.h"
#include "esp_codec_dev.h"
#include "bsp_board.h"
#include <string.h>
#include <math.h>
#define FRAME_MS         20                      // 20 ms audio frame
#define SAMPLE_RATE      16000                   // 16 kHz audio
#define SAMPLE_SIZE      2                       // 16-bit PCM = 2 bytes
#define CHANNELS         1                       // Mono
#define FRAME_LEN        (SAMPLE_RATE / 1000 * FRAME_MS * SAMPLE_SIZE * CHANNELS) // 640 bytes




#include "peer_connection.h"
#define ES8311_RESET_GPIO 46
#define SDA_GPIO          6
#define SCL_GPIO          7
#define I2S_CLK_GPIO 42
#define I2S_DATA_GPIO 41
#define I2S_NUM         I2S_NUM_0
#define I2S_BCK_IO      42
#define I2S_WS_IO       41
#define I2S_DI_IO       2
#define I2S_DO_IO       4 // OUT: Unused if you're only capturing audio
#define ES8311_I2C_ADDR 0x18 

static const char* TAG = "AUDIO";

static es8311_handle_t es8311_dev = NULL;
esp_codec_dev_handle_t record_handle = get_record_handle();
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

    // esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
    // esp_audio_enc_register_default();

    // g711_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_8K;
    // g711_cfg.channel = ESP_AUDIO_MONO;
    // g711_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    // g711_cfg.frame_duration = 10;

    // enc_cfg.type = ESP_AUDIO_TYPE_G711A;
    // enc_cfg.cfg = &g711_cfg;
    // enc_cfg.cfg_sz = sizeof(g711_cfg);

    // ESP_LOGI(TAG, "Initializing encoder: G711A");
    // ESP_LOGI(TAG, "Encoder config: %d Hz, %d ch, %d bits, %d ms frame",
    //          g711_cfg.sample_rate, g711_cfg.channel, g711_cfg.bits_per_sample, g711_cfg.frame_duration);
    // ESP_LOGI(TAG, "Free heap before encoder open: %d", esp_get_free_heap_size());

    // ret = esp_audio_enc_open(&enc_cfg, &enc_handle);
    // if (ret != ESP_AUDIO_ERR_OK) {
    //     ESP_LOGE(TAG, "Encoder open failed: %d", ret);
    //     return ESP_FAIL;
    // }

    bsp_board_init();
    esp_audio_enc_get_frame_size(record_handle, &read_size, &out_size);

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


gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << ES8311_RESET_GPIO),
    .mode = GPIO_MODE_OUTPUT,
};



// void i2c_scan() {
//     ESP_LOGI("I2C", "Scanning I2C bus...");

//     for (uint8_t addr = 1; addr < 127; addr++) {
//          ESP_LOGI("I2C", "Scanning I2C bus... lol");
//         i2c_cmd_handle_t cmd = i2c_cmd_link_create();
//         i2c_master_start(cmd);
//         i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
//         i2c_master_stop(cmd);
//         esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
//         ESP_LOGI("I2C", "Logs for ret %d", ret);
//         i2c_cmd_link_delete(cmd);

//         if (ret == ESP_OK) {
//             ESP_LOGI("I2C", "Found device at 0x%02X", addr);
//         }
//     }
// }
// esp_err_t audio_init(void) {
//     i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
//     ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

//     i2s_pdm_rx_config_t pdm_rx_cfg = {
//         .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(8000),
//         .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
//         .gpio_cfg = {
//             .clk = I2S_CLK_GPIO,
//             .din = I2S_DATA_GPIO,
//             .invert_flags = {
//                 .clk_inv = false,
//             },
//         },
//     };


//     ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg));
//     ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

//     return audio_codec_init();
// }



// esp_err_t audio_init(void) {
//     // 1. Initialize I2C for ES8311
//     i2c_config_t i2c_cfg = {
//         .mode = I2C_MODE_MASTER,
//         .sda_io_num = SDA_GPIO,
//         .scl_io_num = SCL_GPIO,
//         .sda_pullup_en = GPIO_PULLUP_ENABLE,
//         .scl_pullup_en = GPIO_PULLUP_ENABLE,
//         .master.clk_speed = 50000,
//     };
//     ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &i2c_cfg));
//     ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));
//  gpio_set_direction(ES8311_RESET_GPIO, GPIO_MODE_OUTPUT);
// gpio_set_level(ES8311_RESET_GPIO, 0);
// vTaskDelay(pdMS_TO_TICKS(10));
// gpio_set_level(ES8311_RESET_GPIO, 1);
// vTaskDelay(pdMS_TO_TICKS(50));
//   //  i2c_scan();

//     // 2. Initialize I2S
//     i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
//     ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

//     i2s_std_config_t std_cfg = {
//         .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(8000),
//         .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
//         .gpio_cfg = {
//             .mclk = -1,                 // Not using MCLK
//             .bclk = I2S_BCK_IO,
//             .ws   = I2S_WS_IO,
//             .din  = I2S_DI_IO,
//             .dout = I2S_DO_IO,
//         },
//     };
//     ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
//     ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

//   es8311_clock_config_t clk_cfg = {
//     .mclk_from_mclk_pin = false,
//     .sample_frequency = 8000,
// };
// es8311_dev = es8311_create(I2C_NUM_0, 0x18);
// ESP_ERROR_CHECK(es8311_init(es8311_dev, &clk_cfg,
//                             ES8311_RESOLUTION_16,
//                             ES8311_RESOLUTION_16));
// i2c_cmd_handle_t cmd = i2c_cmd_link_create();
// i2c_master_start(cmd);
// i2c_master_write_byte(cmd, (0x18 << 1) | I2C_MASTER_WRITE, true);
// i2c_master_write_byte(cmd, 0x00, true);               // Write to register 0x00
// i2c_master_write_byte(cmd, 0x3F, true);               // Write data 0x3F
// i2c_master_stop(cmd);
// esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
// i2c_cmd_link_delete(cmd);
// ESP_LOGI("I2C_TEST", "Register write status: %d", esp_err_to_name(ret));

//     int actual_vol;
//     ESP_ERROR_CHECK(es8311_voice_volume_set(es8311_dev, 80, &actual_vol));

//     // 4. Initialize encoder
//     return audio_codec_init();
// }
esp_codec_dev_handle_t record_dev = NULL;

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

// void audio_task(void* arg) {
//     int ret;
//     static int64_t last_time, last_log_time;
//     int64_t curr_time;
//     float bytes = 0;

//     last_time = get_timestamp();
//     last_log_time = last_time;
//     ESP_LOGI(TAG, "audio task started");

//     for (;;) {
//         if (eState == PEER_CONNECTION_COMPLETED) {
//             // 💡 Add a yield at the start to reset WDT even if blocked before
//             taskYIELD();  // or esp_task_wdt_reset();

//             esp_codec_dev_handle_t record_handle = get_record_handle();
//             if (record_handle) {
//                 uint8_t buffer[FRAME_LEN] = {0};
// aenc_in_frame.buffer = buffer;
// aenc_in_frame.len = FRAME_LEN;
//                 int ret = esp_codec_dev_read(record_handle, buffer , FRAME_LEN);
//                 if (ret == aenc_in_frame.len) {
//                     // Optional yield between major steps
//                     taskYIELD();

//                     esp_audio_err_t enc_ret = esp_audio_enc_process(enc_handle, &aenc_in_frame, &aenc_out_frame);
//                     if (enc_ret == ESP_AUDIO_ERR_OK) {
//                         int send_ret = peer_connection_send_audio(g_pc, aenc_out_frame.buffer, aenc_out_frame.encoded_bytes);
//                         if (send_ret < 0) {
//                             ESP_LOGW(TAG, "peer_connection_send_audio failed: %d", send_ret);
//                         } else {
//                             ESP_LOGI(TAG, "Sent audio: %d bytes", aenc_out_frame.encoded_bytes);
//                         }
//                         bytes += aenc_out_frame.encoded_bytes;
//                     } else {
//                         ESP_LOGE(TAG, "Audio encode failed: %d", enc_ret);
//                     }
//                 } else {
//                     ESP_LOGW(TAG, "Partial audio frame: %d/%d bytes", ret, aenc_in_frame.len);
//                 }
//             } else {
//                 ESP_LOGE(TAG, "Record handle is NULL");
//             }

//             curr_time = get_timestamp();
//             if ((curr_time - last_log_time) > 5000) {
//                 float bitrate = 1000.0 * (bytes * 8.0 / (curr_time - last_time));
//                 ESP_LOGI(TAG, "audio bitrate: %.1f bps | free heap: %d | stack watermark: %d",
//                          bitrate, esp_get_free_heap_size(), uxTaskGetStackHighWaterMark(NULL));
//                 last_time = curr_time;
//                 last_log_time = curr_time;
//                 bytes = 0;
//             }

//             // 💡 Keep a small delay to avoid hogging CPU
//             vTaskDelay(pdMS_TO_TICKS(5));
//         } else {
//             ESP_LOGD(TAG, "PeerConnection not ready (state=%d), skipping audio send", eState);
//             vTaskDelay(pdMS_TO_TICKS(100));
//         }
//     }
// }
// // G.711 A-law encoding tables and functions for KORVO
// static const uint8_t alaw_compress_table[128] = {
//     1,1,2,2,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
//     6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
//     7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
//     7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
// };

// static uint8_t linear_to_alaw(int16_t pcm_val) {
//     int mask;
//     int seg;
//     uint8_t aval;

//     if (pcm_val >= 0) {
//         mask = 0xD5;
//     } else {
//         mask = 0x55;
//         pcm_val = -pcm_val - 8;
//     }

//     if (pcm_val > 32635) pcm_val = 32635;

//     if (pcm_val < 256) {
//         aval = pcm_val >> 4;
//     } else {
//         pcm_val >>= 4;
//         seg = alaw_compress_table[(pcm_val >> 4) & 0x7F];
//         aval = (seg << 4) | ((pcm_val >> (seg + 3)) & 0x0F);
//     }

//     return aval ^ mask;
// }

// static esp_err_t init_simple_g711_encoder(void) {
//     ESP_LOGI(TAG, "G.711 A-law encoder ready for KORVO");
//     enc_handle = (esp_audio_enc_handle_t)0x12345678; // Dummy handle
//     return ESP_OK;
// }
// esp_codec_dev_handle_t get_record_handle() {
//     return bsp_get_record_dev();;
// }
// void audio_task(void* arg) {
//     ESP_LOGI(TAG, "KORVO audio task started");

//     // Initialize simple G.711 encoder for KORVO
//     esp_err_t ret = init_simple_g711_encoder();
//     if (ret != ESP_OK) {
//         ESP_LOGE(TAG, "Failed to initialize G.711 encoder");
//         vTaskDelete(NULL);
//         return;
//     }

//     int64_t last_time = get_timestamp();
//     int64_t last_log_time = last_time;
//     float bytes_sent = 0;
    
//     // Dynamic frame size detection
//     static bool frame_size_detected = false;
//     static int detected_frame_size = FRAME_LEN;
//     static int read_attempts = 0;
    
//     // Buffering for partial frames
//     static uint8_t partial_buffer[FRAME_LEN * 4] = {0}; // Larger buffer for safety
//     static int partial_bytes = 0;
    
//     // Frame monitoring
//     static int frame_count = 0;
//     static int partial_frame_count = 0;
//     static int consecutive_partial_frames = 0;
//     static int total_bytes_read = 0;

//     for (;;) {
//         if (eState == PEER_CONNECTION_COMPLETED) {
//             taskYIELD();  // Feed WDT

//             
//             if (!record_handle) {
//                 ESP_LOGE(TAG, "Record handle is NULL");
//                 vTaskDelay(pdMS_TO_TICKS(100));
//                 continue;
//             }
            
//             // Auto-detect frame size in first few attempts
//             if (!frame_size_detected && read_attempts < 20) {
//                 uint8_t temp_buffer[FRAME_LEN * 2] = {0};
//                 // int ret = esp_codec_dev_read(record_handle, temp_buffer, FRAME_LEN * 2);
//                 int ret = bsp_get_feed_data(true,temp_buffer,FRAME_LEN * 2);
//                 read_attempts++;

//                 printf("readt sttempdcdcedfvc : %d \n ", ret);
                
//                 if (ret > 0) {
//                     total_bytes_read += ret;
//                     ESP_LOGI(TAG, "Frame size detection: read %d bytes (attempt %d, total: %d)", 
//                              ret, read_attempts, total_bytes_read);
                    
//                     // Common frame sizes: 160, 320, 640, 1280 bytes
//                     if (ret == 160 || ret == 320 || ret == 640 || ret == 1280) {
//                         detected_frame_size = ret;
//                         frame_size_detected = true;
//                         ESP_LOGI(TAG, "Detected frame size: %d bytes", detected_frame_size);
//                     }
//                 }
                
//                 if (read_attempts >= 20) {
//                     // Default to most common read size if no consistent size found
//                     int avg_size = total_bytes_read / read_attempts;
//                     if (avg_size > 0) {
//                         detected_frame_size = avg_size;
//                         ESP_LOGI(TAG, "Using average frame size: %d bytes", detected_frame_size);
//                     }
//                     frame_size_detected = true;
//                 }
                
//                 vTaskDelay(pdMS_TO_TICKS(50));
//                 continue;
//             }

//             uint8_t buffer[FRAME_LEN] = {0};
            
//             // Read using detected frame size, but limit to our buffer
//             int bytes_to_read = (detected_frame_size <= FRAME_LEN) ? detected_frame_size : FRAME_LEN;
//             int ret = esp_codec_dev_read(record_handle, buffer, bytes_to_read);
            
//             if (ret < 0) {
//                 ESP_LOGE(TAG, "esp_codec_dev_read failed: %d", ret);
//                 vTaskDelay(pdMS_TO_TICKS(10));
//                 continue;
//             }

//             if (ret == 0) {
//                 ESP_LOGD(TAG, "No audio data available");
//                 vTaskDelay(pdMS_TO_TICKS(5));
//                 continue;
//             }

//             // Check if we got the expected amount of data
//             if (ret == bytes_to_read) {
//                 // Complete frame received
//                 consecutive_partial_frames = 0;
//                 frame_count++;
                
//                 // Prepare frame for encoding
//                 aenc_in_frame.buffer = buffer;
//                 aenc_in_frame.len = ret;
                
//                 ESP_LOGD(TAG, "Complete audio frame ready: %d bytes", ret);
                
//                 // Process the frame with G.711 encoding
//                 taskYIELD();
                
//                 // G.711 A-law encoding for KORVO
//                 uint8_t encoded_buffer[FRAME_LEN / 2]; // G.711 compresses 16-bit to 8-bit
//                 int16_t* pcm_samples = (int16_t*)buffer;
//                 int sample_count = ret / 2; // 16-bit samples
                
//                 // Encode PCM to G.711 A-law
//                 for (int i = 0; i < sample_count; i++) {
//                     encoded_buffer[i] = linear_to_alaw(pcm_samples[i]);
//                 }
                
//                 int encoded_size = sample_count; // G.711 is 1 byte per sample
                
//                 int send_ret = peer_connection_send_audio(g_pc, encoded_buffer, encoded_size);
//                 if (send_ret < 0) {
//                     ESP_LOGW(TAG, "peer_connection_send_audio failed: %d", send_ret);
//                 } else {
//                     ESP_LOGD(TAG, "Sent G.711 encoded audio: %d bytes (from %d PCM bytes)", encoded_size, ret);
//                 }
//                 bytes_sent += encoded_size;
                
//             } else {
//                 // Partial frame - handle accumulation
//                 partial_frame_count++;
//                 consecutive_partial_frames++;
                
//                 // Prevent buffer overflow
//                 if (partial_bytes + ret > sizeof(partial_buffer)) {
//                     ESP_LOGW(TAG, "Partial buffer overflow, resetting");
//                     partial_bytes = 0;
//                     consecutive_partial_frames = 0;
//                     continue;
//                 }
                
//                 // Accumulate partial data
//                 memcpy(partial_buffer + partial_bytes, buffer, ret);
//                 partial_bytes += ret;
                
//                 // Check if we have enough for a frame
//                 if (partial_bytes >= bytes_to_read) {
//                     // Extract one frame worth of data
//                     memcpy(buffer, partial_buffer, bytes_to_read);
                    
//                     // Shift remaining data
//                     int remaining = partial_bytes - bytes_to_read;
//                     if (remaining > 0) {
//                         memmove(partial_buffer, partial_buffer + bytes_to_read, remaining);
//                     }
//                     partial_bytes = remaining;
                    
//                     // Process the extracted frame
//                     aenc_in_frame.buffer = buffer;
//                     aenc_in_frame.len = bytes_to_read;
                    
//                     taskYIELD();
                    
//                     // G.711 encode the partial frame data
//                     uint8_t encoded_buffer[FRAME_LEN / 2];
//                     int16_t* pcm_samples = (int16_t*)buffer;
//                     int sample_count = bytes_to_read / 2;
                    
//                     for (int i = 0; i < sample_count; i++) {
//                         encoded_buffer[i] = linear_to_alaw(pcm_samples[i]);
//                     }
                    
//                     int encoded_size = sample_count;
//                     int send_ret = peer_connection_send_audio(g_pc, encoded_buffer, encoded_size);
//                     if (send_ret >= 0) {
//                         bytes_sent += encoded_size;
//                         ESP_LOGD(TAG, "Sent G.711 audio from partial buffer: %d bytes", encoded_size);
//                     }
                    
//                     frame_count++;
//                     consecutive_partial_frames = 0;
//                 } else {
//                     // Log partial frame info (reduce frequency)
//                     if (partial_frame_count % 10 == 1) {
//                         float partial_rate = (float)partial_frame_count / (frame_count + partial_frame_count) * 100;
//                         ESP_LOGW(TAG, "Accumulating partial frame: %d/%d bytes (%.1f%% partial rate)", 
//                                  partial_bytes, bytes_to_read, partial_rate);
//                     }
//                 }
                
//                 // Reset if too many consecutive partial frames
//                 if (consecutive_partial_frames > 50) {
//                     ESP_LOGW(TAG, "Too many consecutive partial frames, resetting buffer");
//                     partial_bytes = 0;
//                     consecutive_partial_frames = 0;
//                 }
//             }

//             // Logging and statistics
//             int64_t curr_time = get_timestamp();
//             if ((curr_time - last_log_time) > 5000) {
//                 float bitrate = 1000.0f * (bytes_sent * 8.0f / (curr_time - last_time));
//                 float partial_rate = (frame_count + partial_frame_count > 0) ? 
//                                    (float)partial_frame_count / (frame_count + partial_frame_count) * 100 : 0;
                
//                 ESP_LOGI(TAG, "Audio stats - bitrate: %.1f bps | frames: %d | partial: %.1f%% | detected frame size: %d | free heap: %d",
//                          bitrate, frame_count, partial_rate, detected_frame_size, esp_get_free_heap_size());
                
//                 // Reset counters
//                 last_time = curr_time;
//                 last_log_time = curr_time;
//                 bytes_sent = 0;
//                 frame_count = 0;
//                 partial_frame_count = 0;
//             }

//             // Small delay to prevent overwhelming the codec
//             vTaskDelay(pdMS_TO_TICKS(1));
            
//         } else {
//             ESP_LOGD(TAG, "PeerConnection not ready (state=%d), skipping audio send", eState);
//             // Reset buffers when connection not ready
//             partial_bytes = 0;
//             frame_size_detected = false;
//             read_attempts = 0;
//             total_bytes_read = 0;
//             vTaskDelay(pdMS_TO_TICKS(100));
//         }
//     }
// }

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

                esp_audio_err_t enc_ret = esp_audio_enc_process(record_handle, &aenc_in_frame, &aenc_out_frame);
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
// void audio_task(void* arg) {
//     ESP_LOGI(TAG, "Starting OPUS test tone sender");

//     // Generate 480Hz sine wave: 480 samples @ 48kHz = 10ms
//     int16_t test_buffer[480];
//     for (int i = 0; i < 480; i++) {
//         test_buffer[i] = (int16_t)(16000 * sinf(2 * M_PI * 480 * i / 48000));
//     }

//     while (1) {
//         if (eState == PEER_CONNECTION_COMPLETED && g_pc != NULL) {
//             int ret = peer_connection_send_audio(g_pc, (uint8_t*)test_buffer, 960);
//             ESP_LOGI(TAG, "Sent test tone (960 bytes), ret = %d", ret);
//         } else {
//             ESP_LOGW(TAG, "Peer connection not ready");
//         }

//         vTaskDelay(pdMS_TO_TICKS(10)); // 10ms interval
//     }
// }