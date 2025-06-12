#ifndef VIDEOSDK_AUDIO_H
#define VIDEOSDK_AUDIO_H
#include "esp_err.h"
#include "esp_log.h"



esp_err_t videosdk_audio_codec_init();
esp_err_t videosdk_audio_init();
void videosdk_audio_deinit();
int32_t videosdk_audio_get_samples(uint8_t* buf, size_t size);
void videosdk_audio_task(void* arg);


#endif
