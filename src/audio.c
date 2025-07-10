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

#if defined(CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD)
// #include "opus/opus.h"

// #define SAMPLE_RATE 16000         // OPUS typically uses 16kHz or 48kHz
// #define CHANNELS 2               // Mono
// #define MAX_FRAME_SIZE 960       // 20ms @ 48kHz = 960 samples per channel
static adec_handle_t g_adec = NULL;

static const int16_t ALawDecompressTable[256] = {
    -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736,
    -7552, -7296, -8064, -7808, -6528, -6272, -7040, -6784,
    -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368,
    -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392,
    -22016,-20992,-24064,-23040,-17920,-16896,-19968,-18944,
    -30208,-29184,-32256,-31232,-26112,-25088,-28160,-27136,
    -11008,-10496,-12032,-11520,-8960, -8448, -9984, -9472,
    -15104,-14592,-16128,-15616,-13056,-12544,-14080,-13568,
    -344,  -328,  -376,  -360,  -280,  -264,  -312,  -296,
    -472,  -456,  -504,  -488,  -408,  -392,  -440,  -424,
    -88,   -72,   -120,  -104,  -24,   -8,    -56,   -40,
    -216,  -200,  -248,  -232,  -152,  -136,  -184,  -168,
    -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184,
    -1888, -1824, -2016, -1952, -1632, -1568, -1760, -1696,
    -688,  -656,  -752,  -720,  -560,  -528,  -624,  -592,
    -944,  -912,  -1008, -976,  -816,  -784,  -880,  -848,
     5504,  5248,  6016,  5760,  4480,  4224,  4992,  4736,
     7552,  7296,  8064,  7808,  6528,  6272,  7040,  6784,
     2752,  2624,  3008,  2880,  2240,  2112,  2496,  2368,
     3776,  3648,  4032,  3904,  3264,  3136,  3520,  3392,
    22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944,
    30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136,
    11008, 10496, 12032, 11520, 8960,  8448,  9984,  9472,
    15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568,
     344,   328,   376,   360,   280,   264,   312,   296,
     472,   456,   504,   488,   408,   392,   440,   424,
     88,    72,    120,   104,   24,    8,     56,    40,
     216,   200,   248,   232,   152,   136,   184,   168,
    1376,  1312,  1504,  1440,  1120,  1056,  1248,  1184,
    1888,  1824,  2016,  1952,  1632,  1568,  1760,  1696,
     688,   656,   752,   720,   560,   528,   624,   592,
     944,   912,  1008,   976,   816,   784,   880,   848
};

static inline int16_t alaw_decode(uint8_t a_val) {
    return ALawDecompressTable[a_val];
}
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
esp_g711_enc_config_t g711_cfg;
esp_audio_enc_config_t enc_cfg;
i2s_chan_handle_t rx_handle = NULL;



static uint8_t* read_buf = NULL;
static uint8_t* write_buf = NULL;

// esp_codec_dev_handle_t get_record_handle() {
//     return bsp_get_record_dev();
// }

esp_err_t audio_av_render_init()
{
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

    // videodemo
    // av_render_cfg_t render_cfg = {
    //     .audio_render = player_sys.audio_render,
    //     .audio_raw_fifo_size = 4096,
    //     .audio_render_fifo_size = 6 * 1024,
    //     .video_raw_fifo_size = 500 * 1024,
    //     .allow_drop_data = false,
    //     //.video_render_fifo_size = 4*1024,
    // };


esp_codec_dev_set_out_vol(i2s_cfg.play_handle, 100);
 av_render_cfg_t render_cfg = {
        .audio_render = player_sys.audio_render,
        .audio_raw_fifo_size = 8 * 4096,
        .audio_render_fifo_size = 100 * 1024,
        .allow_drop_data = false,
    };

    player_sys.player = av_render_open(&render_cfg);

    av_render_audio_info_t audio_info = {
        .codec = AV_RENDER_AUDIO_CODEC_G711A,
        .sample_rate = 8000,      // 8000 or 16000 based on your setup
        .channel = 1,             // 1 for mono
        .bits_per_sample = 16,
    };
      av_render_audio_frame_info_t aud_info = {
        .sample_rate = 8000,
        .channel = 1,
        .bits_per_sample = 16,
    };
    av_render_set_fixed_frame_info(player_sys.player, &aud_info);

    int ret = av_render_add_audio_stream(player_sys.player, &audio_info);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to add audio stream to av_render (%d)", ret);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AV Render initialized and stream added");
    return ESP_OK;
}


// esp_err_t audio_playback_init()
// {
//     if (tx_handle != NULL) return ESP_OK;  // Already initialized

//     i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
//     ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));  // Only TX

//     i2s_std_config_t tx_cfg = {
//         .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
//         .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
//         .gpio_cfg = {
//             .mclk = I2S_GPIO_UNUSED,
//            .bclk = I2S_CLK_GPIO,
// .ws   = I2S_WS_GPIO,
// .dout = I2S_DOUT_GPIO,
//             .din = I2S_GPIO_UNUSED,
//             .invert_flags = {
//                 .bclk_inv = false,
//                 .ws_inv = false
//             },
//         },
//     };

//     ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_cfg));
//     ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

//     // Create audio queue and task
//     if (audio_queue == NULL) {
//         audio_queue = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(AudioFrame_t));
//         assert(audio_queue != NULL);
//     }

   
//     ESP_LOGI(TAG, "Audio playback initialized");
//     return ESP_OK;
// }

// static void audio_frame_callback(av_render_audio_frame_t *frame, void *ctx) {
//     const int16_t *pcm_data = (const int16_t *)frame->data;
//     size_t length_bytes = frame->size;

//     if (length_bytes > 0) {
//         esp_err_t err = bsp_audio_play(pcm_data, length_bytes, portMAX_DELAY);
//         if (err != ESP_OK) {
//             ESP_LOGE(TAG, "Audio playback failed: %s", esp_err_to_name(err));
//         }
//     }
// }
// esp_err_t audio_playback_init()
// {
//     if (player_sys.audio_render != NULL) return ESP_OK;  // Already initialized

//     // Fill I2S config struct for av_render
//  printf("i2s_render_init: play_handle=%p\n", get_playback_handle());
//     if (get_playback_handle() == NULL) {
//         ESP_LOGE(TAG, "Playback handle is NULL");
//         return ESP_FAIL;
//     }
//  i2s_render_cfg_t i2s_cfg = {
//         .play_handle = get_playback_handle(),
//     };

// printf("Free heap: %lu", esp_get_free_heap_size());
// printf("Largest free block: %u", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
// player_sys.audio_render = av_render_alloc_i2s_render(&i2s_cfg);
// if (!player_sys.audio_render) {
//     printf("Failed to allocate audio render handle");
//     return ESP_FAIL;
// }

//     // Set stream format (must match your actual audio)
//     av_render_audio_info_t render_info = {
//         .codec = AV_RENDER_AUDIO_CODEC_PCM,   // Already decoded from G711A → PCM16
//         .sample_rate = 8000,
//         .channel = 1,
//     };

//     int ret = av_render_add_audio_stream(player_sys.audio_render, &render_info);
//     if (ret != 0) {
//         ESP_LOGE(TAG, "Failed to add audio stream (ret=%d)", ret);
//         return ESP_FAIL;
//     }

//     ESP_LOGI(TAG, "Audio playback (via av_render) initialized");
//     return ESP_OK;
// }

// void audio_decoder_init_from_codec(av_render_audio_codec_t codec) {
//     adec_cfg_t cfg = {
//         .audio_info = {
//             .codec = codec,          // e.g., AV_RENDER_AUDIO_CODEC_G711A
//             .sample_rate = 8000,
//             .channel = 1,
//             .bits_per_sample = 16,
//         },
//         .frame_cb = audio_frame_callback,
//         .ctx = NULL,
//     };

//     g_adec = adec_open(&cfg);
//     if (!g_adec) {
//         ESP_LOGE(TAG, "Failed to open decoder");
//     } else {
//         ESP_LOGI(TAG, "Decoder initialized for codec %d", codec);
//     }
// }


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
    g711_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_8K;
    g711_cfg.channel = 1;
    g711_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    g711_cfg.frame_duration = 20;

    enc_cfg.type = ESP_AUDIO_TYPE_G711A;
    enc_cfg.cfg = &g711_cfg;
    enc_cfg.cfg_sz = sizeof(g711_cfg);

#if CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD
   
    // // Initialize board peripherals (I2C, I2S, ES8311 and ES7210)
    // if (bsp_board_init(SAMPLE_RATE, CHANNELS, 16) != ESP_OK) {
    //     ESP_LOGE(TAG, "Board init failed");
    //     return ESP_FAIL;
    // }
    
   //  audio_playback_init();
    //  audio_av_render_init();
    record_handle = get_record_handle();
    if (!record_handle) {
        ESP_LOGE(TAG, "Failed to get record handle");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Record handle initialized: %p", record_handle);

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

    ESP_LOGI(TAG, "Audio codec init done. Read size: %d, Out size: %d", read_size, out_size);
    return ESP_OK;
}

// esp_err_t audio_decode_init()
// {
//     printf("Audio decode init\n");

//     // Step 1: Register decoders
//      // Must be called before using any decoder

//     // Step 2: Optional - Initialize the board (you may want to move this out if already done)
//     if (bsp_board_init(SAMPLE_RATE, CHANNELS, 16) != ESP_OK) {
//         ESP_LOGE(TAG, "Board init failed");
//         return ESP_FAIL;
//     }

//     // Step 3: Fill audio format info
//     av_render_audio_info_t audio_info = {
//         .codec        = AV_RENDER_AUDIO_CODEC_G711A,  // or OPUS, etc.
//         .sample_rate  = SAMPLE_RATE,                 // 8000 or 16000
//         .channel      = CHANNELS,                    // usually 1 (mono)
//         .bits_per_sample = 16,
//     };

//     // Step 4: Decoder configuration
//     adec_cfg_t cfg = {
//         .audio_info = audio_info,
//         .frame_cb   = audio_frame_callback,  // Already defined in your code
//         .ctx        = NULL,
//     };

//     // Step 5: Open the decoder
//     g_adec = adec_open(&cfg);
//     if (!g_adec) {
//         ESP_LOGE(TAG, "Failed to open decoder");
//         return ESP_FAIL;
//     }

//     ESP_LOGI(TAG, "Audio decoder initialized for codec %d", audio_info.codec);
//     return ESP_OK;
// }


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
    // int32_t audio_get_samples(uint8_t* buf, size_t size) {
    //     esp_err_t err = esp_codec_dev_read(record_handle, (char*)buf, size);
    //     if (err != ESP_OK) {
    //         ESP_LOGE(TAG, "Codec read error: %d", err);
    //         return -1;
    //     }
    //     return size;
    // }
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

// void audio_task(void* arg) {
//     int ret;
//     static int64_t last_time, last_log_time;
//     static int64_t last_patch_time = 0;  // ✅ Persist across loop iterations
//     int64_t curr_time;
//     float bytes_sent = 0;

//     last_time = get_timestamp();
//     last_log_time = last_time;

//     ESP_LOGI(TAG, "Audio task started");

//     for (;;) {
//         if (eState == PEER_CONNECTION_COMPLETED) {
//             curr_time = get_timestamp();

//             // // ✅ Call PATCH every 15 seconds
//             // if ((curr_time - last_patch_time) > 15000) {
//             //     printf("Sending periodic PATCH\n");
//             //     peer_signaling_send_periodic_patch();
//             //     last_patch_time = curr_time;
//             // }

//             taskYIELD();

//             ret = audio_get_samples(aenc_in_frame.buffer, aenc_in_frame.len);
//             if (ret == aenc_in_frame.len) {
//                 taskYIELD();

//                 esp_audio_err_t enc_ret = esp_audio_enc_process(enc_handle, &aenc_in_frame, &aenc_out_frame);
//                 if (enc_ret == ESP_AUDIO_ERR_OK) {
//                     int send_ret = peer_connection_send_audio(g_pc, aenc_out_frame.buffer, aenc_out_frame.encoded_bytes);
//                     if (send_ret >= 0) {
//                         bytes_sent += aenc_out_frame.encoded_bytes;
//                     }
//                 }
//             }

//             if ((curr_time - last_log_time) > 5000) {
//                 float bitrate = 1000.0f * (bytes_sent * 8.0f / (curr_time - last_time));
//                 // ESP_LOGI(TAG, "Bitrate: %.1f bps | Free heap: %d", bitrate, esp_get_free_heap_size());
//                 last_time = curr_time;
//                 last_log_time = curr_time;
//                 bytes_sent = 0;
//             }

//             vTaskDelay(pdMS_TO_TICKS(5));
//         } else {
//             vTaskDelay(pdMS_TO_TICKS(100));
//         }
//     }
// }

// void audio_playback_task(void *arg)
// {
//     printf("Audio playback task started");
//     AudioFrame_t frame;
//     // Sanity check
//     if (audio_queue == NULL) {
//         ESP_LOGE(TAG, "audio_queue is NULL. Did you forget to call audio_playback_init()?");
//         vTaskDelete(NULL);
//         return;
//     }
// printf("Audio playback task started\n");
//     for (;;) {
//         printf("Waiting for audio frame...\n");
//         // Wait for audio frames from queue
//         printf("frame size: %d\n", sizeof(AudioFrame_t));
//         printf("frame data: %p\n", frame.data);
//         printf("xQueueReceive: %d\n", xQueueReceive(audio_queue, &frame, portMAX_DELAY));
//         printf("pdTRUE: %d\n", pdTRUE);

//         if (xQueueReceive(audio_queue, &frame, portMAX_DELAY) == pdTRUE) {
//             // Ensure alignment and casting
//             const int16_t *pcm_data = (const int16_t *)frame.data;
//             int length_bytes = frame.length;
//             printf("Playing audio frame of length %d bytes\n", length_bytes);

//             esp_err_t err = bsp_audio_play(pcm_data, length_bytes, portMAX_DELAY);
//             if (err != ESP_OK) {
//                 ESP_LOGE(TAG, "bsp_audio_play failed: %s", esp_err_to_name(err));
//             }
//         }
//     }
// }
// void audio_playback_task(void *arg)
// {
//     printf("Audio playback task started\n");

//     AudioFrame_t frame;

//     if (audio_queue == NULL) {
//         ESP_LOGE(TAG, "audio_queue is NULL. Did you forget to call audio_playback_init()?");
//         vTaskDelete(NULL);
//         return;
//     }

//     for (;;) {
//         UBaseType_t queue_len = uxQueueMessagesWaiting(audio_queue);
//         printf("Audio queue length: %u\n", queue_len);
//         printf("Waiting for audio frame...\n");

//         if (xQueueReceive(audio_queue, &frame, pdMS_TO_TICKS(2000)) == pdTRUE) {
//             const int16_t *pcm_data = (const int16_t *)frame.data;
//             int length_bytes = frame.length;

//             printf("Playing audio frame of length %d bytes\n", length_bytes);

//             esp_err_t err = bsp_audio_play(pcm_data, length_bytes, portMAX_DELAY);
//             if (err != ESP_OK) {
//                 ESP_LOGE(TAG, "bsp_audio_play failed: %s", esp_err_to_name(err));
//             }
//         } else {
//             printf("No frame received. Playing test tone.\n");

//             static int16_t test_tone[8000];
//             static bool tone_initialized = false;

//             if (!tone_initialized) {
//                 for (int i = 0; i < 8000; i++) {
//                     test_tone[i] = (int16_t)(3000 * sinf(2 * M_PI * 440.0f * i / 16000));
//                 }
//                 tone_initialized = true;
//             }

//             bsp_audio_play(test_tone, sizeof(test_tone), portMAX_DELAY);
//         }
//     }
// }


// void audio_receive_g711a_and_render(const uint8_t* encoded_data, size_t encoded_len) {
//     if (encoded_len > AUDIO_FRAME_MAX_SIZE / 2) {
//         ESP_LOGW(TAG, "Received oversized G711A frame: %d bytes", (int)encoded_len);
//         return;
//     }

//     static int16_t pcm_buffer[AUDIO_FRAME_MAX_SIZE];  // PCM16 output

//     for (size_t i = 0; i < encoded_len; i++) {
//         pcm_buffer[i] = alaw_decode(encoded_data[i]);
//     }

//     av_render_audio_data_t audio_data = {
//         .data = (uint8_t *)pcm_buffer,
//         .size = encoded_len * sizeof(int16_t),
//     };

//     // int ret = av_render_add_audio_data(player_sys.audio_render, &audio_data);
//     // if (ret != 0) {
//     //     ESP_LOGW(TAG, "Audio render failed (ret=%d)", ret);
//     // } else {
//     //     ESP_LOGI(TAG, "Rendered %d PCM samples", (int)encoded_len);
//     // }

// }


void audio_receive_g711a_and_render(const uint8_t* encoded_data, size_t encoded_len, uint32_t timestamp)  {

    printf("audio_receive_g711a_and_render: encoded_len=%d, timestamp=%ld\n", (int)encoded_len, timestamp);
    if (encoded_len > AUDIO_FRAME_MAX_SIZE / 2) {
        ESP_LOGW(TAG, "Received oversized G711A frame: %d bytes", (int)encoded_len);
        return;
    }

    // static int16_t pcm_buffer[AUDIO_FRAME_MAX_SIZE];  // PCM16 output

    // for (size_t i = 0; i < encoded_len; i++) {
    //     pcm_buffer[i] = alaw_decode(encoded_data[i]);
    // }

    av_render_audio_data_t audio_data = { // Use the provided timestamp
        .data = encoded_data,
        .size = encoded_len * sizeof(int16_t),
    };

    int ret = av_render_add_audio_data(player_sys.player, &audio_data);
    if (ret != 0) {
        ESP_LOGW(TAG, "Audio render failed (ret=%d)", ret);
    } else {
        ESP_LOGI(TAG, "Rendered %d PCM samples", (int)encoded_len);
    }
}



// static OpusDecoder* opus_decoder = NULL;

// void init_opus_decoder_if_needed() {
//     if (!opus_decoder) {
//         int err;
//         opus_decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
//         if (err != OPUS_OK) {
//             ESP_LOGE(TAG, "Failed to create Opus decoder: %s", opus_strerror(err));
//         } else {
//             ESP_LOGI(TAG, "Opus decoder initialized");
//         }
//     }
// }

// void audio_receive_opus_and_render(const uint8_t* encoded_data, size_t encoded_len) {
//     init_opus_decoder_if_needed();  // Ensure decoder is ready

//     if (!opus_decoder) {
//         ESP_LOGE(TAG, "Opus decoder not initialized.");
//         return;
//     }

//     static int16_t pcm_buffer[MAX_FRAME_SIZE];  // PCM16 output buffer

//     int decoded_samples = opus_decode(
//         opus_decoder,
//         encoded_data,
//         encoded_len,
//         pcm_buffer,
//         MAX_FRAME_SIZE,  // number of samples (not bytes)
//         0                // decode FEC = 0 (off)
//     );

//     if (decoded_samples < 0) {
//         ESP_LOGE(TAG, "Opus decode error: %s", opus_strerror(decoded_samples));
//         return;
//     }

//     av_render_audio_data_t audio_data = {
//         .data = (uint8_t *)pcm_buffer,
//         .size = decoded_samples * sizeof(int16_t),  // samples × 2 bytes/sample
//     };

//     // Send to audio renderer
//     // int ret = av_render_add_audio_data(player_sys.audio_render, &audio_data);
//     // if (ret != 0) {
//     //     ESP_LOGW(TAG, "Audio render failed (ret=%d)", ret);
//     // } else {
//     //     ESP_LOGI(TAG, "Rendered %d PCM samples", decoded_samples);
//     // }
// }
// void audio_receive_opus_and_render(const uint8_t* encoded_data, size_t encoded_len) {
//     if (!opus_decoder) {
//         ESP_LOGE(TAG, "Opus decoder not initialized");
//         return;
//     }

//     static int16_t pcm_buffer[960];  // Enough for 20ms @ 48kHz mono

//     int frame_size = opus_decode(opus_decoder,
//                                  encoded_data,
//                                  encoded_len,
//                                  pcm_buffer,
//                                  sizeof(pcm_buffer) / sizeof(pcm_buffer[0]),
//                                  0);
//     if (frame_size < 0) {
//         ESP_LOGE(TAG, "Opus decode failed: %s", opus_strerror(frame_size));
//         return;
//     }

//     av_render_audio_data_t audio_data = {
//         .data = (uint8_t *)pcm_buffer,
//         .size = frame_size * sizeof(int16_t),
//     };

//     // Send to audio render
//     // av_render_add_audio_data(player_sys.audio_render, &audio_data);
// }


// void audio_receive_rtp_payload_and_decode(const uint8_t* encoded_data, size_t encoded_len, int64_t pts) {
//     if (!g_adec) {
//         ESP_LOGW(TAG, "Decoder not initialized");
//         return;
//     }

//     av_render_audio_data_t frame = {
//         .data = (uint8_t *)encoded_data,
//         .size = encoded_len,
//         .pts = pts,
//         .eos = false
//     };

//     adec_decode(g_adec, &frame);
// }