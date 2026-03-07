/*
 * RTP Opus Stream - Modified RTP client for Opus encoded audio over multicast
 *
 * Based on rtp_client_stream.c but adapted for Opus:
 * - No PCM byte-swapping (Opus data is compressed, not raw samples)
 * - Adds 2-byte big-endian frame length prefix for raw_opus_decoder compatibility
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "audio_mem.h"
#include "rtp_opus_stream.h"
#include "esp_heap_caps.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

static const char *TAG = "OPUS_LAN";


#define CONNECT_TIMEOUT_MS        100
#define MULTICAST_TTL 1

typedef enum
{
    JBUF_IDLE=0,
    JBUF_FILLING,
    JBUF_PLAYING
} JBUF_STATE;

typedef struct {
    uint8_t *buf;
    uint32_t w_p;
    uint32_t r_p;
    uint32_t bufsize;
    uint32_t read_thres;
    uint32_t min_thres;
    JBUF_STATE state;
} jbuffer;

typedef struct rtp_opus_stream {
    audio_stream_type_t           type;
    int                           sock;
    int                           port;
    char                          *host;
    bool                          is_open;
    rtp_opus_stream_event_handle_cb    hook;
    void                          *ctx;
    jbuffer                       jbuf;
    bool                          is_multicast;
    char                          prev_host[16];
    int64_t                       last_packet_time;
    int                           sample_rate;
    int                           bits_per_sample;
} rtp_opus_stream_t;

struct rtp_header {
    unsigned int version:2;
    unsigned int padding:1;
    unsigned int extension:1;
    unsigned int cc:4;
    unsigned int marker:1;
    unsigned int pt:7;
    uint16_t seq:16;
    uint32_t ts;
    uint32_t ssrc;
    uint32_t csrc[1];
};

// ///////////////// Multicast socket helpers /////////////////

static int socket_add_ipv4_multicast_group(int sock, bool assign_source_if, char* host)
{
    struct ip_mreq imreq = { 0 };
    struct in_addr iaddr = { 0 };
    int err = 0;

    imreq.imr_interface.s_addr = IPADDR_ANY;

    err = inet_aton(host, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(TAG, "Configured IPV4 multicast address '%s' is invalid.", host);
        err = -1;
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));
    if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
        ESP_LOGW(TAG, "Configured IPV4 multicast address '%s' is not a valid multicast address.", host);
    }

    if (assign_source_if) {
        err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iaddr, sizeof(struct in_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Failed to set IP_MULTICAST_IF. Error %d", errno);
            goto err;
        }
    }

    err = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IP_ADD_MEMBERSHIP. Error %d", errno);
        goto err;
    }

 err:
    return err;
}

static int socket_leave_ipv4_multicast_group(int sock, char* host)
{
    struct ip_mreq imreq = { 0 };
    int err = 0;

    if (sock < 0) {
        ESP_LOGW(TAG, "Invalid socket %d for leaving multicast group", sock);
        return -1;
    }

    err = inet_aton(host, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(TAG, "Configured IPV4 multicast address '%s' is invalid for leaving.", host);
        err = -1;
        goto err;
    }

    imreq.imr_interface.s_addr = IPADDR_ANY;

    err = setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IP_DROP_MEMBERSHIP for %s. Error %d", host, errno);
        goto err;
    }

    ESP_LOGI(TAG, "Left IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));

 err:
    return err;
}

static int create_multicast_ipv4_socket(char* host, int port)
{
    struct sockaddr_in saddr = { 0 };
    int sock = -1;
    int err = 0;

    sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket. Error %d", errno);
        return -1;
    }

    saddr.sin_family = PF_INET;
    saddr.sin_port = htons(port);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to bind socket. Error %d", errno);
        goto err;
    }

    uint8_t ttl = MULTICAST_TTL;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IP_MULTICAST_TTL. Error %d", errno);
        goto err;
    }

    err = socket_add_ipv4_multicast_group(sock, true, host);
    if (err < 0) {
        goto err;
    }

    return sock;

err:
    close(sock);
    return -1;
}

static int socket_switch_multicast_group(int sock, char* old_host, char* new_host)
{
    int err = 0;

    if (sock < 0) {
        ESP_LOGE(TAG, "Invalid socket %d for multicast group switch", sock);
        return -1;
    }

    if (old_host != NULL && strlen(old_host) > 0 && strcmp(old_host, new_host) != 0) {
        err = socket_leave_ipv4_multicast_group(sock, old_host);
        if (err < 0) {
            ESP_LOGW(TAG, "Warning: Failed to leave old multicast group %s, continuing...", old_host);
        }
    }

    err = socket_add_ipv4_multicast_group(sock, true, new_host);
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to join new multicast group %s", new_host);
        return err;
    }

    ESP_LOGI(TAG, "Successfully switched from multicast group %s to %s",
             old_host ? old_host : "(none)", new_host);

    return 0;
}

// ///////////////// Jitter buffer /////////////////

static int jbuf_free(jbuffer *jbuf)
{
    free(jbuf->buf);
    return 0;
}

static int jbuf_init(jbuffer *jbuf, uint32_t size)
{
    jbuf->buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jbuf->buf == NULL) {
        jbuf->buf = malloc(size);  /* fallback to internal if no PSRAM */
    }
    assert(jbuf->buf != NULL);

    memset(jbuf->buf, 0, size);
    jbuf->bufsize = size;
    jbuf->r_p = 0;
    jbuf->w_p = 0;   /* Start empty — zeros are NOT valid Opus frames */
    /*
     * Opus at 32kbps ~= 4100 bytes/sec in jitter buffer.
     * read_thres: initial fill before playback starts (~200ms = ~820 bytes)
     * min_thres:  go back to filling only when nearly empty (~50ms = ~200 bytes)
     * This prevents the fill-drain-fill stutter cycle.
     */
    jbuf->read_thres = 1024;
    jbuf->min_thres = 256;
    jbuf->state = JBUF_IDLE;
    return 0;
}

static int jbuf_write(uint8_t *datain, uint32_t size, jbuffer *jbuf)
{
    uint32_t i = 0;
    while (size) {
        uint32_t new_w_p = jbuf->w_p + 1;
        if (new_w_p >= jbuf->bufsize) new_w_p = 0;
        if (new_w_p == jbuf->r_p) break;  /* buffer full */

        jbuf->buf[jbuf->w_p] = datain[i]; /* store at current pos, THEN advance */
        jbuf->w_p = new_w_p;
        i++;
        size--;
    }
    return i;
}

static int jbuf_read(uint8_t *dataout, uint32_t size, jbuffer *jbuf)
{
    uint32_t i = 0;
    while (size) {
        if (jbuf->r_p == jbuf->w_p) break;
        dataout[i] = jbuf->buf[jbuf->r_p];

        uint32_t new_r_p = jbuf->r_p + 1;
        if (new_r_p >= jbuf->bufsize) new_r_p = 0;

        jbuf->r_p = new_r_p;
        i++;
        size--;
    }
    return i;
}

static int jbuf_count(jbuffer *jbuf)
{
    if (jbuf->r_p == jbuf->w_p) return 0;
    return jbuf->r_p < jbuf->w_p ? jbuf->w_p - jbuf->r_p : jbuf->bufsize - (jbuf->r_p - jbuf->w_p);
}

static int jbuf_ready(jbuffer *jbuf)
{
    return jbuf_count(jbuf) > jbuf->read_thres;
}

// ///////////////// Audio element callbacks /////////////////

static int _get_socket_error_code_reason(const char *str, int sockfd)
{
    uint32_t optlen = sizeof(int);
    int result;
    int err;

    err = getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &result, &optlen);
    if (err == -1) {
        ESP_LOGE(TAG, "%s, getsockopt failed (%d)", str, err);
        return -1;
    }
    if (result != 0) {
        ESP_LOGW(TAG, "%s error, error code: %d, reason: %s", str, err, strerror(result));
    }
    return result;
}

static esp_err_t _dispatch_event(audio_element_handle_t el, rtp_opus_stream_t *rtp, void *data, int len, rtp_opus_stream_status_t state)
{
    if (el && rtp && rtp->hook) {
        rtp_opus_stream_event_msg_t msg = { 0 };
        msg.data = data;
        msg.data_len = len;
        msg.sock_fd = rtp->sock;
        msg.source = el;
        return rtp->hook(&msg, state, rtp->ctx);
    }
    return ESP_FAIL;
}

static esp_err_t _rtp_opus_open(audio_element_handle_t self)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    rtp_opus_stream_t *rtp = (rtp_opus_stream_t *)audio_element_getdata(self);
    if (rtp->is_open) {
        ESP_LOGE(TAG, "Already opened");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Host is %s, port is %d\n", rtp->host, rtp->port);
    int msock = create_multicast_ipv4_socket(rtp->host, rtp->port);
    if (msock < 0) {
        ESP_LOGE(TAG, "Failed to create IPv4 multicast socket");
        return ESP_FAIL;
    }

    int flags = fcntl(msock, F_GETFL, 0);
    fcntl(msock, F_SETFL, flags | O_NONBLOCK);

    rtp->sock = msock;
    if (rtp->sock < 0) {
        _get_socket_error_code_reason(__func__, rtp->sock);
        goto _exit;
    }
    rtp->is_open = true;
    rtp->last_packet_time = esp_timer_get_time();
    _dispatch_event(self, rtp, NULL, 0, RTP_OPUS_STREAM_STATE_CONNECTED);

    return ESP_OK;

_exit:
    return ESP_FAIL;
}

static esp_err_t _rtp_opus_close(audio_element_handle_t self)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    rtp_opus_stream_t *rtp = (rtp_opus_stream_t *)audio_element_getdata(self);
    AUDIO_NULL_CHECK(TAG, rtp, return ESP_FAIL);
    if (!rtp->is_open) {
        ESP_LOGE(TAG, "Already closed");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Shutting down socket");
    shutdown(rtp->sock, 0);
    close(rtp->sock);

    rtp->is_open = false;
    if (AEL_STATE_PAUSED != audio_element_get_state(self)) {
        audio_element_set_byte_pos(self, 0);
    }
    return ESP_OK;
}

static esp_err_t _rtp_opus_destroy(audio_element_handle_t self)
{
    ESP_LOGD(TAG, "_rtp_opus_destroy");
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    rtp_opus_stream_t *rtp = (rtp_opus_stream_t *)audio_element_getdata(self);
    AUDIO_NULL_CHECK(TAG, rtp, return ESP_FAIL);

    jbuf_free(&rtp->jbuf);
    audio_free(rtp);
    return ESP_OK;
}

static uint8_t *opus_packet_buf = NULL;
#define OPUS_PACKET_BUF_SIZE 1024

static esp_err_t _rtp_opus_read(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    rtp_opus_stream_t *rtp = (rtp_opus_stream_t *)audio_element_getdata(self);

    struct sockaddr_in source_addr;
    socklen_t addr_len = sizeof(source_addr);

    int ret = 0;
    static uint16_t seq = 0;

    /*
     * Simple approach: receive one RTP packet, return one complete Opus frame
     * with 2-byte big-endian length prefix. No jitter buffer needed — the
     * pipeline's own ringbuffers between elements handle buffering.
     * This preserves Opus frame boundaries which the decoder requires.
     */
    while (1) {
        if (audio_element_is_stopping(self)) {
            ESP_LOGW(TAG, "_rtp_opus_read: Received stop command, aborting.");
            _rtp_opus_close(self);
            _rtp_opus_destroy(self);
            return ESP_FAIL;
        }

        ret = recvfrom(rtp->sock, opus_packet_buf, OPUS_PACKET_BUF_SIZE, 0,
                       (struct sockaddr *)&source_addr, &addr_len);

        if (ret > 0) {
            if (ret < 12) {
                ESP_LOGW(TAG, "Packet too small (%d bytes), skipping", ret);
                continue;
            }

            rtp->last_packet_time = esp_timer_get_time();

            uint16_t tseq = opus_packet_buf[2] << 8 | opus_packet_buf[3];
            if ((uint16_t)tseq == (uint16_t)seq) {
                continue;  /* duplicate */
            }
            if ((uint16_t)tseq != (uint16_t)(seq + 1)) {
                ESP_LOGW(TAG, "Missing packet! expected %d got %d", seq + 1, tseq);
            }
            seq = tseq;

            uint16_t opus_frame_len = ret - 12;
            int total = 2 + opus_frame_len;

            if (total > len) {
                ESP_LOGW(TAG, "Frame too large (%d > %d), skipping", total, len);
                continue;
            }

            /* 2-byte big-endian length prefix + opus payload */
            buffer[0] = (opus_frame_len >> 8) & 0xFF;
            buffer[1] = opus_frame_len & 0xFF;
            memcpy(buffer + 2, opus_packet_buf + 12, opus_frame_len);

            audio_element_update_byte_pos(self, total);
            return total;

        } else if (ret < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            } else {
                ESP_LOGE(TAG, "_rtp_opus_read: recvfrom error: %d (%s)", errno, strerror(errno));
                return ESP_FAIL;
            }
        }
    }
}

static esp_err_t _rtp_opus_write(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    return ESP_FAIL;
}

static esp_err_t _rtp_opus_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    int r_size = audio_element_input(self, in_buffer, in_len);
    if (r_size <= 0) {
        return r_size;
    }
    /* Write ALL data, retrying on partial writes to preserve Opus frame boundaries */
    int written = 0;
    while (written < r_size) {
        int w_size = audio_element_output(self, in_buffer + written, r_size - written);
        if (w_size <= 0) {
            return w_size;
        }
        written += w_size;
    }
    audio_element_update_byte_pos(self, r_size);
    return r_size;
}

audio_element_handle_t rtp_opus_stream_init(rtp_opus_stream_cfg_t *config)
{
    AUDIO_NULL_CHECK(TAG, config, return NULL);

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    audio_element_handle_t el;
    cfg.open = _rtp_opus_open;
    cfg.close = _rtp_opus_close;
    cfg.process = _rtp_opus_process;
    cfg.destroy = _rtp_opus_destroy;
    cfg.task_stack = config->task_stack;
    cfg.task_prio = config->task_prio;
    cfg.task_core = config->task_core;
    cfg.stack_in_ext = config->ext_stack;
    cfg.tag = "rtp_opus";
    cfg.buffer_len = (config->buf_size > 0) ? config->buf_size : RTP_OPUS_STREAM_BUF_SIZE;

    rtp_opus_stream_t *rtp = audio_calloc(1, sizeof(rtp_opus_stream_t));
    AUDIO_MEM_CHECK(TAG, rtp, return NULL);

    rtp->type = config->type;
    rtp->port = config->port;
    rtp->host = config->host;
    rtp->sample_rate = (config->sample_rate > 0) ? config->sample_rate : RTP_OPUS_STREAM_DEFAULT_SAMPLE_RATE;
    rtp->bits_per_sample = (config->bits_per_sample > 0) ? config->bits_per_sample : RTP_OPUS_STREAM_DEFAULT_BITS_PER_SAMPLE;
    rtp->is_multicast = (config->host != NULL && strlen(config->host) > 0) ? true : false;
    if (config->host) {
        strncpy(rtp->prev_host, config->host, sizeof(rtp->prev_host) - 1);
        rtp->prev_host[sizeof(rtp->prev_host) - 1] = '\0';
    } else {
        rtp->prev_host[0] = '\0';
    }

    if (config->event_handler) {
        rtp->hook = config->event_handler;
        if (config->event_ctx) {
            rtp->ctx = config->event_ctx;
        }
    }

    if (config->type == AUDIO_STREAM_READER) {
        cfg.read = _rtp_opus_read;
    }

    el = audio_element_init(&cfg);
    AUDIO_MEM_CHECK(TAG, el, goto _rtp_opus_init_exit);
    audio_element_setdata(el, rtp);

    /* Allocate packet receive buffer in PSRAM */
    if (opus_packet_buf == NULL) {
        opus_packet_buf = heap_caps_malloc(OPUS_PACKET_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (opus_packet_buf == NULL) {
            opus_packet_buf = malloc(OPUS_PACKET_BUF_SIZE);
        }
        assert(opus_packet_buf != NULL);
    }

    /* Jitter buffer in PSRAM for Opus compressed frames */
    jbuf_init(&rtp->jbuf, 8192);

    return el;

_rtp_opus_init_exit:
    audio_free(rtp);
    return NULL;
}

esp_err_t rtp_opus_stream_switch_multicast_address(audio_element_handle_t self, const char* new_host, uint16_t new_port)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    rtp_opus_stream_t *rtp = (rtp_opus_stream_t *)audio_element_getdata(self);
    AUDIO_NULL_CHECK(TAG, rtp, return ESP_FAIL);

    if (!rtp->is_open) {
        ESP_LOGE(TAG, "RTP opus stream is not open, cannot switch multicast address");
        return ESP_FAIL;
    }

    if (!rtp->is_multicast) {
        ESP_LOGE(TAG, "Current stream is not multicast, cannot switch");
        return ESP_FAIL;
    }

    if (rtp->sock < 0) {
        ESP_LOGE(TAG, "RTP opus stream socket is invalid");
        return ESP_FAIL;
    }

    char old_host[16];
    if (rtp->host != NULL) {
        strncpy(old_host, rtp->host, sizeof(old_host) - 1);
        old_host[sizeof(old_host) - 1] = '\0';
    } else {
        old_host[0] = '\0';
    }

    strncpy(rtp->prev_host, old_host, sizeof(rtp->prev_host) - 1);
    rtp->prev_host[sizeof(rtp->prev_host) - 1] = '\0';

    rtp->host = (char*)new_host;

    if (new_port != 0 && rtp->port != new_port) {
        rtp->port = new_port;
        ESP_LOGW(TAG, "Port changed but socket reconfiguration for port changes not implemented");
    }

    int result = socket_switch_multicast_group(rtp->sock, old_host, (char*)new_host);
    if (result < 0) {
        ESP_LOGE(TAG, "Failed to switch multicast group from %s to %s", old_host, new_host);
        return ESP_FAIL;
    }

    /* Reset jitter buffer to clear old data */
    jbuffer *jbuf = &rtp->jbuf;
    memset(jbuf->buf, 0, jbuf->bufsize);
    jbuf->r_p = 0;
    jbuf->w_p = 0;
    jbuf->state = JBUF_IDLE;

    ESP_LOGI(TAG, "Successfully switched to new multicast address %s", new_host);
    return ESP_OK;
}

esp_err_t rtp_opus_stream_check_connection(audio_element_handle_t self, int timeout_ms)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);
    rtp_opus_stream_t *rtp = (rtp_opus_stream_t *)audio_element_getdata(self);
    AUDIO_NULL_CHECK(TAG, rtp, return ESP_FAIL);

    if (!rtp->is_open) {
        return ESP_FAIL;
    }

    int64_t current_time = esp_timer_get_time();
    if ((current_time - rtp->last_packet_time) > (int64_t)timeout_ms * 1000) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
