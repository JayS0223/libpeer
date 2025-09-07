#ifndef VIDEOSDK_H_
#define VIDEOSDK_H_

#ifdef __cplusplus
extern "C" {
#endif

// enum for the supported codecs
typedef enum {

  AUDIO_CODEC_G711A,
  AUDIO_CODEC_G711U,
  AUDIO_CODEC_OPUS,

} audio_codec_t;

// struct for the init method
typedef struct {
  char* meetingID;
  char* token;
  char* displayName;
  audio_codec_t audioCodec;
} init_config_t;

// enum for the errors
typedef enum {
  RESULT_OK = 0,
  SSL_CONNECT_FAILED = 3001,
  HTTP_REQUEST_FAILED = 3002,
  MEMORY_ALLOC_FAILED = 3003,
  INIT_FAILED = 3004,
  INIT_NULL_PARAMETER = 3005,
  INIT_BOARD_FAILED = 3006,
  PEER_INIT_FAILED = 3008,
  TASK_OVERRIDED = 3009,
  INIT_CODEC_FAILED = 3010,
  PUBLISH_MUTEX_CREATE_FAILED = 3011,
  AUDIO_CODEC_INIT_FAILED = 3012,
  PUBLISH_PEER_CONNECTION_FAILED = 3013,
  PUBLISH_MEMORY_ALLOC_FAILED = 3014,
  PUBLISH_TASK_CREATE_FAILED = 3015,
  SUBSCRIBE_MUTEX_CREATE_FAILED = 3016,
  SUBSCRIBE_PEER_CONNECTION_FAILED = 3017,
  SUBSCRIBE_MEMORY_ALLOC_FAILED = 3018,
  SUBSCRIBE_TASK_CREATE_FAILED = 3019,
  STOP_PUBLISH_TASK_CREATE_FAILED = 3020,
  STOP_SUBSCRIBE_TASK_CREATE_FAILED = 3021,
  CANDIDATE_PAIR_FAILED = 3021,
  DTLS_HANDSHAKE_FAILED = 3022,
  LEVAVE_FAILED = 3023,
  DEVICE_NOT_SUPPORTED = 3024
} result_t;

// struct to return the created Meeting and also the error code
typedef struct {
  result_t code;
  char* room_id;
} create_meeting_result_t;

// create meeting
create_meeting_result_t create_meeting(char* token);
// initialize the meeting
result_t init(init_config_t* cfg);
// Publish Audio
result_t startPublishAudio(char* publisherId);
// subscribe Audio
result_t startSubscribeAudio(char* subscriberId, char* remotePeerId);
// Publish stop
result_t stopPublishAudio();
// Subscribe Audio Stop
result_t stopSubscribeAudio();
// leave method to stop the
result_t leave();

#ifdef __cplusplus
}
#endif

#endif  // VIDEOSDK_H_