#include <stdio.h>
#include "esp_log.h"
#include "codec_init.h"
#include "codec_board.h"
#include "esp_codec_dev.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG_TAG = "Board";

int init_board(void)
{
    ESP_LOGI(TAG_TAG, "Init board.");
    set_codec_board_type(TEST_BOARD_NAME);
    // Notes when use playback and record at same time, must set reuse_dev = false
    codec_init_cfg_t cfg = {
        .in_mode = CODEC_I2S_MODE_TDM,
        .in_use_tdm = true,
        .reuse_dev = false
    };
    if (init_codec(&cfg) != 0) {
        ESP_LOGE(TAG_TAG, "Codec initialization failed");
        return -1;
    }
    return 0;
}
