#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "agent.h"

#include "config.h"
#include "dtls_srtp.h"
#include "peer_connection.h"
#include "ports.h"
#include "rtcp.h"
#include "rtp.h"
#include "sctp.h"
#include <stdbool.h>
#include "sdp.h"

#define STATE_CHANGED(pc, curr_state)                                 \
  if (pc->oniceconnectionstatechange && pc->state != curr_state) {    \
    pc->oniceconnectionstatechange(curr_state, pc->config.user_data); \
    pc->state = curr_state;                                           \
  }

struct PeerConnection {
  PeerConfiguration config;
  PeerConnectionState state;
  Agent agent;
  DtlsSrtp dtls_srtp;
  Sctp sctp;
 time_t last_binding_request_time;
  Sdp local_sdp;
  Sdp remote_sdp;

  void (*onicecandidate)(char* sdp, void* user_data);
  void (*oniceconnectionstatechange)(PeerConnectionState state, void* user_data);
  void (*on_connected)(void* userdata);
  void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* user_data);

  uint8_t temp_buf[CONFIG_MTU];
  uint8_t agent_buf[CONFIG_MTU];
  int agent_ret;
  int b_local_description_created;



  RtpEncoder artp_encoder;
  RtpEncoder vrtp_encoder;
  RtpDecoder vrtp_decoder;
  RtpDecoder artp_decoder;

  uint32_t remote_assrc;
  uint32_t remote_vssrc;
};

bool g_subscribe = false;
bool g_publish = false;
static void peer_connection_outgoing_rtp_packet(uint8_t* data, size_t size, void* user_data) {
  PeerConnection* pc = (PeerConnection*)user_data;
  dtls_srtp_encrypt_rtp_packet(&pc->dtls_srtp, data, (int*)&size);
  agent_send(&pc->agent, data, size);
}

static int peer_connection_dtls_srtp_recv(void* ctx, unsigned char* buf, size_t len) {
  static const int MAX_RECV = 200;
  int recv_max = 0;
  int ret;
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  if (pc->agent_ret > 0 && pc->agent_ret <= len) {
    memcpy(buf, pc->agent_buf, pc->agent_ret);
    return pc->agent_ret;
  }

  while (recv_max < MAX_RECV) {
    ret = agent_recv(&pc->agent, buf, len);

    if (ret > 0) {
      break;
    }

    recv_max++;
  }
  return ret;
}

static int peer_connection_dtls_srtp_send(void* ctx, const uint8_t* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  // LOGD("send %.4x %.4x, %ld", *(uint16_t*)buf, *(uint16_t*)(buf + 2), len);
  return agent_send(&pc->agent, buf, len);
}

static void peer_connection_incoming_rtcp(PeerConnection* pc, uint8_t* buf, size_t len) {
  RtcpHeader* rtcp_header;
  size_t pos = 0;

  while (pos < len) {
    rtcp_header = (RtcpHeader*)(buf + pos);

    switch (rtcp_header->type) {
      case RTCP_RR:
        LOGD("RTCP_PR");
        if (rtcp_header->rc > 0) {
// TODO: REMB, GCC ...etc
#if 0
          RtcpRr rtcp_rr = rtcp_parse_rr(buf);
          uint32_t fraction = ntohl(rtcp_rr.report_block[0].flcnpl) >> 24;
          uint32_t total = ntohl(rtcp_rr.report_block[0].flcnpl) & 0x00FFFFFF;
          if(pc->on_receiver_packet_loss && fraction > 0) {

            pc->on_receiver_packet_loss((float)fraction/256.0, total, pc->config.user_data);
          }
#endif
        }
        break;
      case RTCP_PSFB: {
        int fmt = rtcp_header->rc;
        LOGD("RTCP_PSFB %d", fmt);
        // PLI and FIR
        if ((fmt == 1 || fmt == 4) && pc->config.on_request_keyframe) {
          pc->config.on_request_keyframe(pc->config.user_data);
        }
      }
      default:
        break;
    }

    pos += 4 * ntohs(rtcp_header->length) + 4;
  }
}

const char* peer_connection_state_to_string(PeerConnectionState state) {
  switch (state) {
    case PEER_CONNECTION_NEW:
      return "new";
    case PEER_CONNECTION_CHECKING:
      return "checking";
    case PEER_CONNECTION_CONNECTED:
      return "connected";
    case PEER_CONNECTION_COMPLETED:
      return "completed";
    case PEER_CONNECTION_FAILED:
      return "failed";
    case PEER_CONNECTION_CLOSED:
      return "closed";
    case PEER_CONNECTION_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

PeerConnectionState peer_connection_get_state(PeerConnection* pc) {
  return pc->state;
}

void* peer_connection_get_sctp(PeerConnection* pc) {
  return &pc->sctp;
}

PeerConnection* peer_connection_create(PeerConfiguration* config, bool publish, bool subscribe) {
  printf("Creating peer connection publish %d \n", publish);
   printf("Creating peer connection subscribe %d \n", subscribe);
  PeerConnection* pc = calloc(1, sizeof(PeerConnection));
  if (!pc) {
    return NULL;
  }
    g_subscribe = subscribe;
    g_publish = publish;
  memcpy(&pc->config, config, sizeof(PeerConfiguration));

  agent_create(&pc->agent);

  memset(&pc->sctp, 0, sizeof(pc->sctp));
  
  // Initialize DTLS-SRTP structure to prevent crashes
  memset(&pc->dtls_srtp, 0, sizeof(pc->dtls_srtp));

  if (pc->config.audio_codec) {

    rtp_encoder_init(&pc->artp_encoder, pc->config.audio_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->artp_decoder, pc->config.audio_codec,
                     pc->config.onaudiotrack, pc->config.user_data);
  }

  if (pc->config.video_codec) {


    rtp_encoder_init(&pc->vrtp_encoder, pc->config.video_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->vrtp_decoder, pc->config.video_codec,
                     pc->config.onvideotrack, pc->config.user_data);
  }

  return pc;
}

void peer_connection_destroy(PeerConnection* pc) {
  if (pc) {
    agent_destroy(&pc->agent);
    
    // Clean up DTLS-SRTP resources
    dtls_srtp_deinit(&pc->dtls_srtp);

    free(pc);
    pc = NULL;
  }
}

void peer_connection_close(PeerConnection* pc) {
  pc->state = PEER_CONNECTION_CLOSED;
}

int peer_connection_send_audio(PeerConnection* pc, const uint8_t* buf, size_t len) {
  
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }

    return rtp_encoder_encode(&pc->artp_encoder, buf, len);
}

int peer_connection_send_video(PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }

   return rtp_encoder_encode(&pc->vrtp_encoder, buf, len);
}

int peer_connection_datachannel_send(PeerConnection* pc, char* message, size_t len) {
  return peer_connection_datachannel_send_sid(pc, message, len, 0);
}

int peer_connection_datachannel_send_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid) {
  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return -1;
  }

  if (pc->config.datachannel == DATA_CHANNEL_STRING)
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_STRING, sid);
  else
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_BINARY, sid);
}

static char* peer_connection_dtls_role_setup_value(DtlsSrtpRole d) {
  return "a=setup:passive";
}
void sdp_force_recvonly(Sdp* sdp) {
  // Replace all a=sendrecv or a=sendonly with a=recvonly
  char* p = sdp->content;
  while ((p = strstr(p, "a=sendrecv")) || (p = strstr(p, "a=sendonly"))) {
    memcpy(p, "a=recvonly", strlen("a=recvonly"));
    p += strlen("a=recvonly");
  }
}

char* create_recvonly_offer(PeerConnection* pc) {
  memset(&pc->local_sdp, 0, sizeof(pc->local_sdp));

  // Only set up the SDP with recvonly streams
  sdp_create(&pc->local_sdp,
             pc->config.video_codec != CODEC_NONE,
             pc->config.audio_codec != CODEC_NONE,
             pc->config.datachannel);

  if (pc->config.video_codec == CODEC_H264) {
    sdp_append_h264(&pc->local_sdp);

    // Indicate fingerprint if DTLS is enabled
    sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);

    // Setup attribute for DTLS role
    sdp_append(&pc->local_sdp, "a=setup:actpass");
  }

  // Overwrite all media directions with recvonly
  sdp_force_recvonly(&pc->local_sdp);

  return pc->local_sdp.content;
}


static void peer_connection_state_new(PeerConnection* pc, DtlsSrtpRole role, int isOfferer) {
  printf("peer_connection_state_new\n");
  char* description = (char*)pc->temp_buf;

  memset(pc->temp_buf, 0, sizeof(pc->temp_buf));

  dtls_srtp_reset_session(&pc->dtls_srtp);
  printf("peer_connection_state_new: dtls_srtp reset\n");
  // for(int i = 0; i< 10; i++){
  //   vTaskDelay(pdMS_TO_TICKS(10));
  // }
  vTaskDelay(pdMS_TO_TICKS(180));

  dtls_srtp_init(&pc->dtls_srtp, role, pc);
  printf("peer_connection_state_new: dtls_srtp initialized\n");
  pc->dtls_srtp.udp_recv = peer_connection_dtls_srtp_recv;
  pc->dtls_srtp.udp_send = peer_connection_dtls_srtp_send;

  pc->sctp.connected = 0;

  if (isOfferer) {
    agent_clear_candidates(&pc->agent);
    pc->agent.mode = AGENT_MODE_CONTROLLING;
  } else {
    pc->agent.mode = AGENT_MODE_CONTROLLED;
  }

  agent_gather_candidate(&pc->agent, NULL, NULL, NULL);  // host address
  for (int i = 0; i < sizeof(pc->config.ice_servers) / sizeof(pc->config.ice_servers[0]); ++i) {
    if (pc->config.ice_servers[i].urls) {
      LOGI("ice server: %s", pc->config.ice_servers[i].urls);
      agent_gather_candidate(&pc->agent, pc->config.ice_servers[i].urls, pc->config.ice_servers[i].username, pc->config.ice_servers[i].credential);
    }
  }

  agent_get_local_description(&pc->agent, description, sizeof(pc->temp_buf));

  memset(&pc->local_sdp, 0, sizeof(pc->local_sdp));
  // TODO: check if we have video or audio codecs
  sdp_create(&pc->local_sdp,
             pc->config.video_codec != CODEC_NONE,
             pc->config.audio_codec != CODEC_NONE,
             pc->config.datachannel);

  if (pc->config.video_codec == CODEC_H264) {
    sdp_append_h264(&pc->local_sdp);
    sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
    sdp_append(&pc->local_sdp, peer_connection_dtls_role_setup_value(role));
    strcat(pc->local_sdp.content, description);
  }

  switch (pc->config.audio_codec) {
    case CODEC_PCMA:

      sdp_append_pcma(&pc->local_sdp);
      sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
      sdp_append(&pc->local_sdp, peer_connection_dtls_role_setup_value(role));
      strcat(pc->local_sdp.content, description);
      break;

    case CODEC_PCMU:

      sdp_append_pcmu(&pc->local_sdp);
      sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
      sdp_append(&pc->local_sdp, peer_connection_dtls_role_setup_value(role));
      strcat(pc->local_sdp.content, description);
      break;

    case CODEC_OPUS:
      sdp_append_opus(&pc->local_sdp);
      sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
      sdp_append(&pc->local_sdp, peer_connection_dtls_role_setup_value(role));
      strcat(pc->local_sdp.content, description);
      break;

    default:
      break;
  }

  if (pc->config.datachannel) {
    sdp_append_datachannel(&pc->local_sdp);
    sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
    sdp_append(&pc->local_sdp, peer_connection_dtls_role_setup_value(role));
    strcat(pc->local_sdp.content, description);
  }

  pc->b_local_description_created = 1;

  if (pc->onicecandidate) {
    pc->onicecandidate(pc->local_sdp.content, pc->config.user_data);
  }
}

// static void peer_connection_state_new(PeerConnection* pc, DtlsSrtpRole role, int isOfferer) {
//   LOGI(">>> Entered peer_connection_state_new");

//   // if (!pc) {
//   //   LOGE("PeerConnection is NULL");
//   //   return;
//   // }

//   // if (!pc->temp_buf) {
//   //   LOGE("pc->temp_buf is NULL");
//   //   return;
//   // }

//   char* description = (char*)pc->temp_buf;
//   memset(pc->temp_buf, 0, sizeof(pc->temp_buf));
//   LOGI("temp_buf cleared");
//   LOGI("checking pc->dtls_srtp ptr: %p", &pc->dtls_srtp);
//   heap_caps_check_integrity_all(true); // optional heap check
  
  
  
//   // Initialize DTLS-SRTP first, then reset if needed
//   int init_result = dtls_srtp_init(&pc->dtls_srtp, role, pc);
//   if (init_result != 0) {
//     LOGE("DTLS-SRTP initialization failed with code %d", init_result);
//     return;
//   }
//   LOGI("DTLS-SRTP session initialized with role %d", role);
//   pc->dtls_srtp.udp_recv = peer_connection_dtls_srtp_recv;
//   pc->dtls_srtp.udp_send = peer_connection_dtls_srtp_send;
//   LOGI("DTLS-SRTP session initialized with udp_recv and udp_send callbacks");

//   pc->sctp.connected = 0;

//   if (isOfferer) {
//     LOGI("Offerer: clearing candidates and setting mode CONTROLLING");
//     agent_clear_candidates(&pc->agent);
//     pc->agent.mode = AGENT_MODE_CONTROLLING;
//   } else {
//     LOGI("Answerer: setting mode CONTROLLED");
//     pc->agent.mode = AGENT_MODE_CONTROLLED;
//   }

//   LOGI("Gathering host candidate");
//   agent_gather_candidate(&pc->agent, NULL, NULL, NULL);  // host address

//   LOGI("Checking ICE servers...");
//   for (int i = 0; i < sizeof(pc->config.ice_servers) / sizeof(pc->config.ice_servers[0]); ++i) {
//     if (pc->config.ice_servers[i].urls) {
//       LOGI("Gathering ICE server candidate: %s", pc->config.ice_servers[i].urls);
//       agent_gather_candidate(&pc->agent, pc->config.ice_servers[i].urls,
//                              pc->config.ice_servers[i].username,
//                              pc->config.ice_servers[i].credential);
//     }
//   }

//   LOGI("Getting local ICE description");
//   agent_get_local_description(&pc->agent, description, sizeof(pc->temp_buf));
//   LOGI("Local ICE description populated");

//   memset(&pc->local_sdp, 0, sizeof(pc->local_sdp));
//   LOGI("SDP cleared");

//   sdp_create(&pc->local_sdp,
//              pc->config.video_codec != CODEC_NONE,
//              pc->config.audio_codec != CODEC_NONE,
//              pc->config.datachannel);
//   LOGI("SDP created (video: %d, audio: %d, dc: %d)",
//         pc->config.video_codec != CODEC_NONE,
//         pc->config.audio_codec != CODEC_NONE,
//         pc->config.datachannel);

//   // if (!pc->local_sdp.content) {
//   //   LOGE("local_sdp.content is NULL — this will crash strcat!");
//   //   return;
//   // }

//   // VIDEO
//   if (pc->config.video_codec == CODEC_H264) {
//     LOGI("Appending H264 to SDP");
//     sdp_append_h264(&pc->local_sdp);
//     sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
//     sdp_append(&pc->local_sdp, peer_connection_dtls_role_setup_value(role));
//     strcat(pc->local_sdp.content, description);
//   }

//   // AUDIO
//   switch (pc->config.audio_codec) {
//     case CODEC_PCMA:
//       LOGI("Appending PCMA to SDP");
//       sdp_append_pcma(&pc->local_sdp);
//       sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
//       sdp_append(&pc->local_sdp, peer_connection_dtls_role_setup_value(role));
//       strcat(pc->local_sdp.content, description);
//       break;

//     case CODEC_PCMU:
//       LOGI("Appending PCMU to SDP");
//       sdp_append_pcmu(&pc->local_sdp);
//       sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
//       sdp_append(&pc->local_sdp, peer_connection_dtls_role_setup_value(role));
//       strcat(pc->local_sdp.content, description);
//       break;

//     case CODEC_OPUS:
//       LOGI("Appending OPUS to SDP");
//       sdp_append_opus(&pc->local_sdp);
//       sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
//       sdp_append(&pc->local_sdp, peer_connection_dtls_role_setup_value(role));
//       strcat(pc->local_sdp.content, description);
//       break;

//     default:
//       LOGI("No matching audio codec");
//       break;
//   }

//   // DATA CHANNEL
//   if (pc->config.datachannel) {
//     LOGI("Appending DataChannel to SDP");
//     sdp_append_datachannel(&pc->local_sdp);
//     sdp_append(&pc->local_sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
//     sdp_append(&pc->local_sdp, peer_connection_dtls_role_setup_value(role));
//     strcat(pc->local_sdp.content, description);
//   }

//   pc->b_local_description_created = 1;
//   LOGI("Local SDP complete");

//   if (pc->onicecandidate) {
//     LOGI("Calling onicecandidate callback");
//     pc->onicecandidate(pc->local_sdp.content, pc->config.user_data);
//   } else {
//     LOGW("onicecandidate callback is NULL");
//   }

//   LOGI("<<< Exiting peer_connection_state_new");
// }



int peer_connection_loop(PeerConnection* pc) {
  printf("peer_connection_loop publish %d \n", g_publish);
   printf("peer_connection_loop subscribe %d \n", g_subscribe);
printf("PeerConnection address: %p\n", (void *)&pc);

  uint32_t ssrc = 0;
  memset(pc->agent_buf, 0, sizeof(pc->agent_buf));
  pc->agent_ret = -1;

  switch (pc->state) {
    case PEER_CONNECTION_NEW:
      printf("PEER_CONNECTION_NEW inside the loop\n");
      if (!pc->b_local_description_created) {
        if (g_subscribe) {
         printf("Creating recvonly offer\n");
          peer_connection_state_new(pc, DTLS_SRTP_ROLE_CLIENT, 0);
        } else if (g_publish) {
            printf("Creating sendrecv offer\n");
          peer_connection_state_new(pc, DTLS_SRTP_ROLE_SERVER, 1);
          printf("peer connection state new\n");
        } else {
          LOGE("Invalid state for peer connection");
          return -1;
        }
      //  peer_connection_state_new(pc, DTLS_SRTP_ROLE_SERVER, 1);
      }else {
        printf("Local description already created\n");
      }
      break;

      case PEER_CONNECTION_CHECKING:
      if (agent_select_candidate_pair(&pc->agent) < 0) {
        STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
      } else if (agent_connectivity_check(&pc->agent, 0) == 0) {
        STATE_CHANGED(pc, PEER_CONNECTION_CONNECTED);
      }
      break;

    case PEER_CONNECTION_CONNECTED:

      if (dtls_srtp_handshake(&pc->dtls_srtp, NULL) == 0) {
        LOGD("DTLS-SRTP handshake done");

        if (pc->config.datachannel) {
          LOGI("SCTP create socket");
          sctp_create_socket(&pc->sctp, &pc->dtls_srtp);
          pc->sctp.userdata = pc->config.user_data;
        }
        pc->last_binding_request_time = time(NULL);
        STATE_CHANGED(pc, PEER_CONNECTION_COMPLETED);
      }
      break;
    case PEER_CONNECTION_COMPLETED:
        LOGI("PEER_CONNECTION_COMPLETED %lld" , pc->last_binding_request_time);
        time_t current_time = time(NULL);
        LOGI("PEER_CONNECTION_COMPLETED %lld" , current_time);
        LOGI("PEER_CONNECTION_COMPLETED %lld" , current_time - pc->last_binding_request_time);
        if (current_time - pc->last_binding_request_time >= 8) {
          agent_connectivity_check(&pc->agent, 1);
          LOGI("heartbeat sent!");
          pc->last_binding_request_time = current_time;
        }

      // data = buffer_peak_head(pc->video_rb, &bytes);
      // if (data) {
      //   rtp_encoder_encode(&pc->vrtp_encoder, data, bytes);
      //   buffer_pop_head(pc->video_rb);
      // }

      // data = buffer_peak_head(pc->audio_rb, &bytes);
      // if (data) {
      //   rtp_encoder_encode(&pc->artp_encoder, data, bytes);
      //   buffer_pop_head(pc->audio_rb);
      // }

      // data = buffer_peak_head(pc->data_rb, &bytes);
      // if (data) {
      //   if (pc->config.datachannel == DATA_CHANNEL_STRING)
      //     sctp_outgoing_data(&pc->sctp, (char*)data, bytes, PPID_STRING, 0);
      //   else
      //     sctp_outgoing_data(&pc->sctp, (char*)data, bytes, PPID_BINARY, 0);
      //   buffer_pop_head(pc->data_rb);
      // }

      if ((pc->agent_ret = agent_recv(&pc->agent, pc->agent_buf, sizeof(pc->agent_buf))) > 0) {
        LOGD("agent_recv %d", pc->agent_ret);

        if (rtcp_probe(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTCP packet");
          int decrypt_status = dtls_srtp_decrypt_rtp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);
          if (decrypt_status != 0) {
              LOGE("SRTP decryption failed with code %d", decrypt_status);
              return 0;
          }
          // dtls_srtp_decrypt_rtcp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);
          peer_connection_incoming_rtcp(pc, pc->agent_buf, pc->agent_ret);

        } else if (dtls_srtp_probe(pc->agent_buf)) {
          int ret = dtls_srtp_read(&pc->dtls_srtp, pc->temp_buf, sizeof(pc->temp_buf));
          LOGD("Got DTLS data %d", ret);

          if (ret > 0) {
            sctp_incoming_data(&pc->sctp, (char*)pc->temp_buf, ret);
          }

        } else if (rtp_packet_validate(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTP packet");

          dtls_srtp_decrypt_rtp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);

          ssrc = rtp_get_ssrc(pc->agent_buf);
          if (ssrc == pc->remote_assrc) {
            rtp_decoder_decode(&pc->artp_decoder, pc->agent_buf, pc->agent_ret);
          } else if (ssrc == pc->remote_vssrc) {
            rtp_decoder_decode(&pc->vrtp_decoder, pc->agent_buf, pc->agent_ret);
          }

        } else {
          LOGW("Unknown data");
        }
      }

      if (KEEPALIVE_CONNCHECK > 0 && (ports_get_epoch_time() - pc->agent.binding_request_time) > KEEPALIVE_CONNCHECK) {
        LOGI("binding request timeout");
        STATE_CHANGED(pc, PEER_CONNECTION_CLOSED);
      }

      break;
    case PEER_CONNECTION_FAILED:
      break;
    case PEER_CONNECTION_DISCONNECTED:
      break;
    case PEER_CONNECTION_CLOSED:
      break;
    default:
      break;
  }

  return 0;
}

void peer_connection_set_remote_description(PeerConnection* pc, const char* sdp_text) {
  char* start = (char*)sdp_text;
  char* line = NULL;
  char buf[256];
  char* val_start = NULL;
  uint32_t* ssrc = NULL;
  DtlsSrtpRole role = DTLS_SRTP_ROLE_SERVER;
  int is_update = 0;
  Agent* agent = &pc->agent;

  while ((line = strstr(start, "\r\n"))) {
    
    line = strstr(start, "\r\n");
    strncpy(buf, start, line - start);
    buf[line - start] = '\0';

    if (strstr(buf, "a=setup:passive")) {
      role = DTLS_SRTP_ROLE_CLIENT;
    }
  
    if (strstr(buf, "a=fingerprint")) {
      strncpy(pc->dtls_srtp.remote_fingerprint, buf + 22, DTLS_SRTP_FINGERPRINT_LENGTH);
    }

    if (strstr(buf, "a=ice-ufrag") &&
        strlen(agent->remote_ufrag) != 0 &&
        (strncmp(buf + strlen("a=ice-ufrag:"), agent->remote_ufrag, strlen(agent->remote_ufrag)) == 0)) {
      is_update = 1;
    }


    if (strstr(buf, "m=video")) {
      ssrc = &pc->remote_vssrc;
    } else if (strstr(buf, "m=audio")) {
      ssrc = &pc->remote_assrc;
    }

    if ((val_start = strstr(buf, "a=ssrc:")) && ssrc) {
      *ssrc = strtoul(val_start + 7, NULL, 10);
      LOGD("SSRC: %" PRIu32, *ssrc);
    }

    start = line + 2;
  }

  if (is_update) {
    return;
  }

  if (!pc->b_local_description_created) {
    peer_connection_state_new(pc, role, 0);
  }


  agent_set_remote_description(&pc->agent, (char*)sdp_text);
  printf("Setting remote description\n");
  STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
}

void peer_connection_create_offer(PeerConnection* pc) {
  printf("Creating peer connection offer\n");
  STATE_CHANGED(pc, PEER_CONNECTION_NEW);
  printf("Below the state changed !!");
  pc->b_local_description_created = 0;
}

int peer_connection_send_rtcp_pil(PeerConnection* pc, uint32_t ssrc) {
  int ret = -1;
  uint8_t plibuf[128];
  rtcp_get_pli(plibuf, 12, ssrc);

  // TODO: encrypt rtcp packet
  // guint size = 12;
  // dtls_transport_encrypt_rctp_packet(pc->dtls_transport, plibuf, &size);
  // ret = nice_agent_send(pc->nice_agent, pc->stream_id, pc->component_id, size, (gchar*)plibuf);

  return ret;
}

// callbacks
void peer_connection_on_connected(PeerConnection* pc, void (*on_connected)(void* userdata)) {
  pc->on_connected = on_connected;
}

void peer_connection_on_receiver_packet_loss(PeerConnection* pc,
                                             void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* userdata)) {
  pc->on_receiver_packet_loss = on_receiver_packet_loss;
}
int counter4 = 0;

void peer_connection_onicecandidate(PeerConnection* pc, void (*onicecandidate)(char* sdp_text, void* userdata)) {
printf("Counter inside the peer_connection_onicecandidate:%d", counter4);
counter4++;
  pc->onicecandidate = onicecandidate;
}

void peer_connection_oniceconnectionstatechange(PeerConnection* pc,
                                                void (*oniceconnectionstatechange)(PeerConnectionState state, void* userdata)) {
  pc->oniceconnectionstatechange = oniceconnectionstatechange;
}

void peer_connection_ondatachannel(PeerConnection* pc,
                                   void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid),
                                   void (*onopen)(void* userdata),
                                   void (*onclose)(void* userdata)) {
  if (pc) {
    sctp_onopen(&pc->sctp, onopen);
    sctp_onclose(&pc->sctp, onclose);
    sctp_onmessage(&pc->sctp, onmessage);
  }
}

int peer_connection_lookup_sid(PeerConnection* pc, const char* label, uint16_t* sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (strncmp(pc->sctp.stream_table[i].label, label, sizeof(pc->sctp.stream_table[i].label)) == 0) {
      *sid = pc->sctp.stream_table[i].sid;
      return 0;
    }
  }
  return -1;  // Not found
}

char* peer_connection_lookup_sid_label(PeerConnection* pc, uint16_t sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (pc->sctp.stream_table[i].sid == sid) {
      return pc->sctp.stream_table[i].label;
    }
  }
  return NULL;  // Not found
}

int peer_connection_add_ice_candidate(PeerConnection* pc, char* candidate) {
  Agent* agent = &pc->agent;
  if (ice_candidate_from_description(&agent->remote_candidates[agent->remote_candidates_count], candidate, candidate + strlen(candidate)) != 0) {
    return -1;
  }

  agent->remote_candidates_count++;
  return 0;
}
