#ifndef VIDEOSDK_H_
#define VIDEOSDK_H_



#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
char* meetingID;
char* token;
char* displayName;
char* participantId;

} init_config_t;

typedef enum {

   AUDIO_CODEC_G711A,  /*!< G711 alaw audio type */
    AUDIO_CODEC_G711U,  /*!< G711 ulaw audio type */
    AUDIO_CODEC_OPUS,   /*!< OPUS audio type */

} audio_codec_t;

typedef struct {

  audio_codec_t codec;
  char*  consumerId;

} subsribe_cfg_t;

typedef struct {
	audio_codec_t codec;
} config_t;

typedef enum {
    RESULT_OK = 0,
    SSL_CONNECT_FAILED = 3001,
    HTTP_REQUEST_FAILED = 3002,
    MEMORY_ALLOC_FAILED = 3003,
    INIT_FAILED = 3004,
    INIT_NULL_PARAMETER = 3005,     
    INIT_BOARD_FAILED = 3006,       
    INIT_PEER_FAILED = 3007,         
    INIT_CODEC_FAILED = 3008,        
    PUBLISH_MUTEX_CREATE_FAILED = 3009, 
    PUBLISH_AUDIO_CODEC_FAILED = 3010,   
    PUBLISH_PEER_CONNECTION_FAILED = 3011,
    PUBLISH_MEMORY_ALLOC_FAILED = 3012,   
    PUBLISH_WHIP_CONNECT_FAILED = 3013,   
    PUBLISH_TASK_CREATE_FAILED = 3014,    
    SUBSCRIBE_MUTEX_CREATE_FAILED = 3015,  
    SUBSCRIBE_AV_RENDER_FAILED = 3016,    
    SUBSCRIBE_PEER_CONNECTION_FAILED = 3017, 
    SUBSCRIBE_MEMORY_ALLOC_FAILED = 3018,   
    SUBSCRIBE_WHEP_CONNECT_FAILED = 3019,   
    SUBSCRIBE_TASK_CREATE_FAILED = 3020,    
    STOP_PUBLISH_TASK_CREATE_FAILED = 3021, 
    LOCAL_DESCRIPTION_ALREADY_CREATED = 3022,
    CANDIDATE_PAIR_FAILED = 3023, 
    DTLS_HANDSHAKE_FAILED = 3024

} result_t;


typedef struct {
    result_t code;   
    char *room_id;   
} create_meeting_result_t;

typedef struct {
    char *token;
    char *customMeetingId;
}create_meeting_config_t;

create_meeting_result_t create_meeting(create_meeting_config_t *meetingConfig_t);

result_t init(init_config_t *cfg);
// char* create_meeting();
result_t startPublishAudio(audio_codec_t cfg);
result_t startSubscribeAudio(audio_codec_t cfg);
result_t stopPublishAudio();

void loop_log();
#ifdef __cplusplus

}
#endif

#endif // VIDEOSDK_H_