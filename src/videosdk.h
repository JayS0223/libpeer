#ifndef VIDEOSDK_H_
#define VIDEOSDK_H_



#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
char* meetingID;
char* token;
char* displayName;
} init_config_t;

typedef enum {

   AUDIO_CODEC_G711A,  /*!< G711 alaw audio type */
    AUDIO_CODEC_G711U,  /*!< G711 ulaw audio type */
    AUDIO_CODEC_OPUS,   /*!< OPUS audio type */

} audio_codec_t;



typedef struct {
	audio_codec_t codec;
} config_t;

typedef enum {
	ENC_REGISTRATION_FAILED,
    I2C_FAILED,
    ENC_OPEN_FAILED, 
    GET_FRAME_SIZE_FAILED,
    READ_DATA_FAILED,
    READ_DATA_SUCCESS,
    ENC_PROCESS_FAILED,
    ENC_PROCESS_SUCCESS,
    SEND_AUDIO_FAILED,
    SEND_AUDIO_SUCCESS,
	DEC_REGISTRATION_FAILED,
    PLAYBACK_HANDLE_NULL,
    PLAYBACK_HANDLE_SUCCESS,
    RENDER_OPEN_FAILED,
    RENDER_OPEN_SUCCESS, 
    SET_FRAME_SIZE_FAILED,
    SET_FRAME_SIZE_SUCCESS,
    RENDER_ADD_STREAM_FAILED,
    RENDER_ADD_STREAM_SUCCESS,
    REMOVE_CUSTOM_HEADER_FAILED,
    REMOVE_CUSTOM_HEADER_SUCCESS,
    ADD_AUDIO_RENDER_DATA_FAILED,
    ADD_AUDIO_RENDER_DATA_SUCCESS,




} result_t;


//char* create_meeting(const char *auth_token);
int init(init_config_t *cfg);
int startPublishAudio(audio_codec_t cfg);
int startSubscribeAudio(audio_codec_t cfg);
void loop_log();
#ifdef __cplusplus

}
#endif

#endif // VIDEOSDK_H_