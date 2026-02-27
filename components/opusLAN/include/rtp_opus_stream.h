/*
 * RTP Opus Stream - Modified RTP client stream for Opus encoded audio
 *
 * Based on rtp_client_stream but without PCM byte-swapping,
 * and with 2-byte frame length prefix for raw Opus decoder compatibility.
 */

#ifndef _RTP_OPUS_STREAM_H_
#define _RTP_OPUS_STREAM_H_

#include "audio_error.h"
#include "audio_element.h"
#include "esp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTP_OPUS_STREAM_STATE_NONE,
    RTP_OPUS_STREAM_STATE_CONNECTED,
} rtp_opus_stream_status_t;

typedef struct rtp_opus_stream_event_msg {
    void                          *source;
    void                          *data;
    int                           data_len;
    esp_transport_handle_t        sock_fd;
} rtp_opus_stream_event_msg_t;

typedef esp_err_t (*rtp_opus_stream_event_handle_cb)(rtp_opus_stream_event_msg_t *msg, rtp_opus_stream_status_t state, void *event_ctx);

typedef struct {
    audio_stream_type_t         type;
    int                         timeout_ms;
    int                         port;
    char                        *host;
    int                         task_stack;
    int                         task_core;
    int                         task_prio;
    bool                        ext_stack;
    int                         buf_size;
    int                         sample_rate;
    int                         bits_per_sample;
    rtp_opus_stream_event_handle_cb  event_handler;
    void                        *event_ctx;
} rtp_opus_stream_cfg_t;

#define RTP_OPUS_STREAM_DEFAULT_PORT             (8080)
#define RTP_OPUS_STREAM_TASK_STACK               (3072)
#define RTP_OPUS_STREAM_BUF_SIZE                 (1024)
#define RTP_OPUS_STREAM_TASK_PRIO                (22)
#define RTP_OPUS_STREAM_TASK_CORE                (0)
#define RTP_OPUS_STREAM_DEFAULT_SAMPLE_RATE      (48000)
#define RTP_OPUS_STREAM_DEFAULT_BITS_PER_SAMPLE  (16)

#define RTP_OPUS_STREAM_CFG_DEFAULT() {                     \
    .type            = AUDIO_STREAM_READER,                 \
    .timeout_ms      = 30 * 1000,                           \
    .port            = RTP_OPUS_STREAM_DEFAULT_PORT,        \
    .host            = NULL,                                \
    .task_stack      = RTP_OPUS_STREAM_TASK_STACK,          \
    .task_core       = RTP_OPUS_STREAM_TASK_CORE,           \
    .task_prio       = RTP_OPUS_STREAM_TASK_PRIO,           \
    .ext_stack       = true,                                \
    .buf_size        = RTP_OPUS_STREAM_BUF_SIZE,            \
    .sample_rate     = RTP_OPUS_STREAM_DEFAULT_SAMPLE_RATE, \
    .bits_per_sample = RTP_OPUS_STREAM_DEFAULT_BITS_PER_SAMPLE, \
    .event_handler   = NULL,                                \
    .event_ctx       = NULL,                                \
}

/**
 * @brief       Initialize an RTP Opus stream reader audio element
 */
audio_element_handle_t rtp_opus_stream_init(rtp_opus_stream_cfg_t *config);

/**
 * @brief       Switch multicast address without restarting pipeline
 */
esp_err_t rtp_opus_stream_switch_multicast_address(audio_element_handle_t self, const char* new_host, uint16_t new_port);

/**
 * @brief       Check if data is being received on the RTP stream
 */
esp_err_t rtp_opus_stream_check_connection(audio_element_handle_t self, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
