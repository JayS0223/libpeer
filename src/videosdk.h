#ifndef VIDEOSDK_H
#define VIDEOSDK_H
#include <stdbool.h> 
#include "esp_err.h"

esp_err_t videosdk_init(const char* meetingId, const char* token, const bool enableMic, const char* displayName);
esp_err_t videosdk_connect();
void videosdk_loop();

#endif

