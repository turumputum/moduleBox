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
    /* RTP clock rate tracking — measures sender Hz, used to adjust I2S clock */
    bool                          sync_initialized;
    uint32_t                      rtp_ts_base;
    int64_t                       local_time_base_us;
    int64_t                       last_clk_update_us;   /* last time measured_sample_hz was updated */
    int32_t                       measured_sample_hz;   /* sender's measured audio clock rate */
    /* Stream identity tracking for restart detection */
    uint32_t                      last_ssrc;
    uint16_t                      rtp_seq;
    bool                          need_seq_seed;       /* true = next packet seeds seq (no loss check) */
    int64_t                       last_overflow_log_us; /* for rate-limiting overflow warnings */
    /* Sync-start: all devices with same latency_ms start at same RTP timestamp position */
    uint32_t                      target_delay_ms;     /* latency target from config */
    uint32_t                      sync_rtp_ts_start;   /* RTP ts threshold set on first packet */
    volatile bool                 sync_ready;          /* set by drain task when ts threshold reached */
    /* Drain task — reads socket independently of pipeline backpressure */
    TaskHandle_t                  drain_task_handle;
    volatile bool                 drain_running;
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

    // Socket receive buffer — Opus frames are small (~80 bytes) so 32KB can
    // hold ~400 packets (~8s).  Larger than audioLAN's 16KB because Opus
    // packets are much smaller than raw PCM and we need to survive WiFi
    // power-save gaps without packet loss.
    int rcvbuf_size = 32768;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));

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

static int create_unicast_ipv4_socket(int port)
{
    struct sockaddr_in saddr = { 0 };
    int sock = -1;
    int err = 0;

    sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create unicast socket. Error %d", errno);
        return -1;
    }

    // Socket receive buffer — match multicast socket's 32KB
    int rcvbuf_size = 32768;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));

    saddr.sin_family = PF_INET;
    saddr.sin_port = htons(port);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to bind unicast socket. Error %d", errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "Unicast socket created and bound to port %d", port);
    return sock;
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
     * Opus at ~80 bytes/frame * 50 fps ≈ 4000 bytes/sec compressed.
     * read_thres: pre-fill before playback starts (~0.5s = ~2000 bytes).
     *             Pipeline burst-drains ~700 bytes to fill downstream
     *             ringbuffers, leaving ~1300 bytes residual (~325ms).
     *             Lower than before (was 6000 = 1.5s) for minimal latency.
     * No min_thres: underrun is detected only when jbuf is truly empty
     *             (no complete frame available). Pipeline + downstream ringbuffers
     *             provide ~170ms of additional buffering in PCM form.
     */
    jbuf->read_thres = 2000;
    jbuf->min_thres = 0;  /* disabled — underrun = empty jbuf, not a threshold */
    jbuf->state = JBUF_IDLE;
    return 0;
}

// ///////////////// end jitter buffer /////////////////

static inline uint32_t jbuf_available(jbuffer *jbuf)
{
    uint32_t w = jbuf->w_p;
    uint32_t r = jbuf->r_p;
    if (w >= r) return w - r;
    return jbuf->bufsize - r + w;
}

static inline uint32_t jbuf_free_space(jbuffer *jbuf)
{
    return jbuf->bufsize - 1 - jbuf_available(jbuf);
}

static int jbuf_write(jbuffer *jbuf, const uint8_t *data, uint32_t len)
{
    if (jbuf_free_space(jbuf) < len) return -1;
    uint32_t w = jbuf->w_p;
    uint32_t first = jbuf->bufsize - w;
    if (first >= len) {
        memcpy(jbuf->buf + w, data, len);
    } else {
        memcpy(jbuf->buf + w, data, first);
        memcpy(jbuf->buf, data + first, len - first);
    }
    __asm__ volatile("" ::: "memory");  /* compiler barrier: data visible before w_p update */
    jbuf->w_p = (w + len) % jbuf->bufsize;
    return 0;
}

static int jbuf_read(jbuffer *jbuf, uint8_t *data, uint32_t len)
{
    if (jbuf_available(jbuf) < len) return -1;
    uint32_t r = jbuf->r_p;
    uint32_t first = jbuf->bufsize - r;
    if (first >= len) {
        memcpy(data, jbuf->buf + r, len);
    } else {
        memcpy(data, jbuf->buf + r, first);
        memcpy(data + first, jbuf->buf, len - first);
    }
    __asm__ volatile("" ::: "memory");
    jbuf->r_p = (r + len) % jbuf->bufsize;
    return 0;
}

static int jbuf_peek(jbuffer *jbuf, uint8_t *data, uint32_t len)
{
    if (jbuf_available(jbuf) < len) return -1;
    uint32_t r = jbuf->r_p;
    uint32_t first = jbuf->bufsize - r;
    if (first >= len) {
        memcpy(data, jbuf->buf + r, len);
    } else {
        memcpy(data, jbuf->buf + r, first);
        memcpy(data + first, jbuf->buf, len - first);
    }
    return 0;
}


// ///////////////// Forward declarations /////////////////

static void rtp_opus_drain_task(void *arg);

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

    int msock;
    if (rtp->is_multicast) {
        ESP_LOGI(TAG, "Opening multicast stream: host=%s, port=%d", rtp->host, rtp->port);
        msock = create_multicast_ipv4_socket(rtp->host, rtp->port);
        if (msock < 0) {
            ESP_LOGE(TAG, "Failed to create IPv4 multicast socket");
            return ESP_FAIL;
        }
    } else {
        ESP_LOGI(TAG, "Opening unicast stream: port=%d", rtp->port);
        msock = create_unicast_ipv4_socket(rtp->port);
        if (msock < 0) {
            ESP_LOGE(TAG, "Failed to create IPv4 unicast socket");
            return ESP_FAIL;
        }
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
    rtp->sync_initialized = false;  /* reset RTP timestamp sync on each open */
    rtp->last_ssrc        = 0;
    rtp->rtp_seq          = 0;
    rtp->need_seq_seed    = true;

    /* Reset jitter buffer */
    memset(rtp->jbuf.buf, 0, rtp->jbuf.bufsize);
    rtp->jbuf.r_p = 0;
    rtp->jbuf.w_p = 0;
    rtp->jbuf.state = JBUF_IDLE;
    rtp->sync_ready = false;          /* wait for RTP sync point before playback */
    rtp->sync_rtp_ts_start = 0;

    /* Start drain task — reads socket into jitter buffer independently.
     * Priority 24 = above audio element tasks — must never miss packets.
     * Core 1 to keep core 0 free for lwIP/Ethernet stack. */
    rtp->drain_running = true;
    xTaskCreatePinnedToCore(rtp_opus_drain_task, "rtp_drain", 4096, self, 24, &rtp->drain_task_handle, 1);
    ESP_LOGI(TAG, "Drain task created (core 1, prio 24)");

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

    /* Stop drain task before closing socket */
    rtp->drain_running = false;
    vTaskDelay(pdMS_TO_TICKS(50));  /* drain task exits within ~5ms */
    rtp->drain_task_handle = NULL;
    ESP_LOGI(TAG, "Drain task stopped");

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
#define OPUS_PACKET_BUF_SIZE 1500  /* Must fit max Opus frame + 12-byte RTP header within MTU */

/* ///////////////// Socket drain task /////////////////
 *
 * Continuously reads RTP packets from the socket and writes Opus frames
 * (with 2-byte length prefix) into the jitter buffer.  Runs independently
 * of the audio pipeline, so pipeline backpressure (audio_element_output
 * blocking on a full downstream ringbuffer) does NOT stall recvfrom().
 */
static void rtp_opus_drain_task(void *arg)
{
    audio_element_handle_t self = (audio_element_handle_t)arg;
    rtp_opus_stream_t *rtp = (rtp_opus_stream_t *)audio_element_getdata(self);

    struct sockaddr_in source_addr;
    socklen_t addr_len = sizeof(source_addr);

    /* Diagnostic tracking */
    int64_t last_recv_us = esp_timer_get_time();
    int64_t last_stats_us = last_recv_us;
    int64_t max_gap_us = 0;         /* longest gap between successful recvfrom calls */
    uint32_t total_packets = 0;
    uint32_t total_lost = 0;

    /* Use blocking socket with timeout instead of non-blocking + vTaskDelay.
     * This way the task sleeps efficiently in lwIP and wakes immediately
     * when a packet arrives — no 2ms polling delay, no CPU waste. */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 50000;  /* 50ms timeout — wake up to check drain_running */
    setsockopt(rtp->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Switch socket back to blocking mode (was set non-blocking in _rtp_opus_open) */
    int flags = fcntl(rtp->sock, F_GETFL, 0);
    fcntl(rtp->sock, F_SETFL, flags & ~O_NONBLOCK);

    ESP_LOGI(TAG, "Drain task started on core %d (blocking mode, 50ms timeout)", xPortGetCoreID());

    int64_t loop_top_us = esp_timer_get_time();  /* timestamp at top of loop iteration */

    while (rtp->drain_running) {
        int64_t before_recv_us = esp_timer_get_time();
        int ret = recvfrom(rtp->sock, opus_packet_buf, OPUS_PACKET_BUF_SIZE, 0,
                           (struct sockaddr *)&source_addr, &addr_len);
        int64_t after_recv_us = esp_timer_get_time();

        if (ret > 0) {
            if (ret < 12) {
                loop_top_us = esp_timer_get_time();
                continue;  /* packet too small, skip */
            }

            int64_t now_us = after_recv_us;
            int64_t gap_us = now_us - last_recv_us;
            last_recv_us = now_us;
            if (gap_us > max_gap_us) max_gap_us = gap_us;

            /* Diagnostic: when gap > 200ms, identify where time was spent */
            if (gap_us > 200000LL) {
                int64_t recv_dur_ms   = (after_recv_us - before_recv_us) / 1000;
                int64_t prerecv_ms    = (before_recv_us - loop_top_us) / 1000;
                ESP_LOGW(TAG, "STALL %lldms: recvfrom=%lldms pre_recv=%lldms jbuf=%lu/%lu",
                         gap_us / 1000, recv_dur_ms, prerecv_ms,
                         (unsigned long)jbuf_available(&rtp->jbuf),
                         (unsigned long)rtp->jbuf.bufsize);
            }
            total_packets++;

            rtp->last_packet_time = now_us;

            /* --- RTP header parsing --- */
            uint32_t rtp_ts = ((uint32_t)opus_packet_buf[4] << 24)
                            | ((uint32_t)opus_packet_buf[5] << 16)
                            | ((uint32_t)opus_packet_buf[6] <<  8)
                            |  (uint32_t)opus_packet_buf[7];

            uint32_t ssrc = ((uint32_t)opus_packet_buf[8]  << 24)
                          | ((uint32_t)opus_packet_buf[9]  << 16)
                          | ((uint32_t)opus_packet_buf[10] <<  8)
                          |  (uint32_t)opus_packet_buf[11];

            /* Detect stream restart: SSRC change */
            if (rtp->sync_initialized && ssrc != rtp->last_ssrc) {
                ESP_LOGI(TAG, "SSRC changed 0x%08lx -> 0x%08lx, re-syncing",
                         (unsigned long)rtp->last_ssrc, (unsigned long)ssrc);
                rtp->sync_initialized  = false;
                rtp->sync_ready        = false;
                rtp->sync_rtp_ts_start = 0;
                rtp->rtp_seq           = 0;
                rtp->need_seq_seed    = true;
                /* Flush jitter buffer — two simultaneous sources would otherwise
                 * fill it at 2× the drain rate and cause permanent overflow. */
                jbuffer *jb = &rtp->jbuf;
                jb->r_p   = 0;
                jb->w_p   = 0;
                jb->state = JBUF_IDLE;
                ESP_LOGI(TAG, "jbuf flushed on SSRC change");
            }
            rtp->last_ssrc = ssrc;

            /* --- Sync-start: mark ready when we reach the target RTP timestamp --- */
            if (!rtp->sync_ready && rtp->sync_rtp_ts_start != 0) {
                if ((int32_t)(rtp_ts - rtp->sync_rtp_ts_start) >= 0) {
                    rtp->sync_ready = true;
                    ESP_LOGI(TAG, "[sync] Ready at RTP ts=%lu (target=%lu), starting playback",
                             (unsigned long)rtp_ts, (unsigned long)rtp->sync_rtp_ts_start);
                }
            }

            /* --- RTP clock rate tracking --- */
            if (!rtp->sync_initialized) {
                rtp->rtp_ts_base            = rtp_ts;
                rtp->local_time_base_us     = now_us;
                rtp->last_clk_update_us     = now_us;
                rtp->measured_sample_hz     = rtp->sample_rate;
                rtp->sync_initialized       = true;
                /* Compute sync-start timestamp: all devices with same latency_ms will wait
                 * for the SAME RTP ts, so they all start playback at the same stream position */
                uint32_t delay_samples = (uint32_t)rtp->target_delay_ms * rtp->sample_rate / 1000;
                rtp->sync_rtp_ts_start = rtp_ts + delay_samples;
                ESP_LOGI(TAG, "RTP clock tracking init: ts_base=%lu sample_rate=%d sync_start_ts=%lu (latency=%dms)",
                         (unsigned long)rtp->rtp_ts_base, rtp->sample_rate,
                         (unsigned long)rtp->sync_rtp_ts_start, (int)rtp->target_delay_ms);
            } else {
                int64_t since_base_us = now_us - rtp->local_time_base_us;

                if (since_base_us > 60000000LL &&
                    (now_us - rtp->last_clk_update_us) > 60000000LL) {

                    int64_t rtp_elapsed = (int32_t)(rtp_ts - rtp->rtp_ts_base);
                    if (rtp_elapsed > 0 && since_base_us > 0) {
                        int32_t hz_raw = (int32_t)(rtp_elapsed * 1000000LL / since_base_us);
                        int32_t max_delta = rtp->sample_rate / 2000;
                        if ((hz_raw - rtp->sample_rate) >  max_delta) hz_raw = rtp->sample_rate + max_delta;
                        if ((rtp->sample_rate - hz_raw)  >  max_delta) hz_raw = rtp->sample_rate - max_delta;

                        int32_t prev = rtp->measured_sample_hz;
                        if (prev == rtp->sample_rate) {
                            rtp->measured_sample_hz = hz_raw;
                        } else {
                            rtp->measured_sample_hz = (int32_t)((int64_t)hz_raw * 15 + (int64_t)prev * 85) / 100;
                        }
                        // RTP clock measured log removed
                    }
                    rtp->last_clk_update_us = now_us;

                    if (since_base_us > 14400000000LL) {
                        rtp->rtp_ts_base        = rtp_ts;
                        rtp->local_time_base_us = now_us;
                        ESP_LOGI(TAG, "RTP clock base rolled forward (4h limit)");
                    }
                }

                int32_t ts_delta = (int32_t)(rtp_ts - rtp->rtp_ts_base);
                if (ts_delta < -(int32_t)(rtp->sample_rate * 5)) {
                    ESP_LOGW(TAG, "RTP timestamp jumped back >5s, re-syncing clock base");
                    rtp->rtp_ts_base        = rtp_ts;
                    rtp->local_time_base_us = now_us;
                    rtp->last_clk_update_us = now_us;
                }
            }

            /* --- Sequence number tracking --- */
            uint16_t tseq = opus_packet_buf[2] << 8 | opus_packet_buf[3];
            if (rtp->need_seq_seed) {
                /* First packet after open or SSRC change — seed seq, no loss check */
                rtp->rtp_seq       = tseq;
                rtp->need_seq_seed = false;
            } else {
                if ((uint16_t)tseq == rtp->rtp_seq) {
                    continue;  /* duplicate */
                }
                if ((uint16_t)tseq != (uint16_t)(rtp->rtp_seq + 1)) {
                    uint16_t lost = (uint16_t)(tseq - rtp->rtp_seq - 1);
                    total_lost += lost;
                    ESP_LOGW(TAG, "Missing %d packets! expected %d got %d (recv_gap=%lldms jbuf=%lu/%lu)",
                             lost, rtp->rtp_seq + 1, tseq,
                             gap_us / 1000,
                             (unsigned long)jbuf_available(&rtp->jbuf),
                             (unsigned long)rtp->jbuf.bufsize);

                    /* PLC: inject zero-length marker frames so the Opus decoder
                     * generates concealment audio instead of silence gaps.
                     * A 2-byte header 0x00 0x00 (frame_len=0) tells the decoder
                     * to call opus_decode(dec, NULL, 0, ...) → PLC output.
                     * Cap at 5 frames (~100ms) to avoid flooding the buffer. */
                    uint16_t plc_count = (lost > 5) ? 5 : lost;
                    uint8_t plc_hdr[2] = { 0x00, 0x00 };
                    for (uint16_t p = 0; p < plc_count; p++) {
                        if (jbuf_free_space(&rtp->jbuf) >= 2) {
                            jbuf_write(&rtp->jbuf, plc_hdr, 2);
                        }
                    }
                    if (plc_count > 0) {
                        ESP_LOGI(TAG, "PLC: injected %d concealment frames", plc_count);
                    }
                }
                rtp->rtp_seq = tseq;
            }

            /* --- Write length-prefixed Opus frame into jitter buffer --- */
            uint16_t opus_frame_len = ret - 12;
            uint32_t total = 2 + opus_frame_len;
            uint8_t hdr[2] = { (opus_frame_len >> 8) & 0xFF, opus_frame_len & 0xFF };

            if (jbuf_free_space(&rtp->jbuf) >= total) {
                jbuf_write(&rtp->jbuf, hdr, 2);
                jbuf_write(&rtp->jbuf, opus_packet_buf + 12, opus_frame_len);
            } else {
                /* Rate-limit overflow warnings to once per second */
                if ((now_us - rtp->last_overflow_log_us) > 1000000LL) {
                    ESP_LOGW(TAG, "Jitter buffer overflow! avail:%lu free:%lu need:%lu",
                             (unsigned long)jbuf_available(&rtp->jbuf),
                             (unsigned long)jbuf_free_space(&rtp->jbuf),
                             (unsigned long)total);
                    rtp->last_overflow_log_us = now_us;
                }
            }

            /* --- Periodic stats (every 60s) --- */
            if ((now_us - last_stats_us) > 60000000LL) {
                // DRAIN stats log removed
                max_gap_us = 0;
                last_stats_us = now_us;
            }

            loop_top_us = esp_timer_get_time();

        } else if (ret < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                loop_top_us = esp_timer_get_time();
                continue;  /* timeout — check drain_running and loop */
            } else {
                ESP_LOGE(TAG, "drain_task recvfrom error: %d (%s)", errno, strerror(errno));
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Drain task exiting");
    vTaskDelete(NULL);
}

/*
 * _rtp_opus_read — pulls one complete Opus frame from the jitter buffer.
 * The drain task fills the jitter buffer from the socket independently.
 *
 * Initial pre-buffering: waits until jbuf reaches read_thres (~6000 bytes,
 * ~1.5s of Opus) before serving any data. This gives the pipeline enough
 * to fill all downstream ringbuffers in one burst.
 *
 * After that: serve frames as fast as pipeline requests.  At startup the
 * pipeline drains jbuf rapidly to fill downstream ringbuffers (~16+16KB
 * of PCM); jbuf will drop to 0 and that is NORMAL — downstream buffers
 * now hold ~200ms of decoded PCM.  In steady state, drain task writes
 * ~80 bytes/20ms and pipeline reads ~80 bytes/20ms → balanced.
 * If jbuf is momentarily empty, we just wait — drain task will deliver
 * the next frame within 20ms, well within downstream buffer margin.
 * No underrun/refill cycling — once PLAYING, stay PLAYING.
 */
static esp_err_t _rtp_opus_read(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    rtp_opus_stream_t *rtp = (rtp_opus_stream_t *)audio_element_getdata(self);
    jbuffer *jbuf = &rtp->jbuf;

    while (1) {
        if (audio_element_is_stopping(self)) {
            return ESP_FAIL;
        }

        uint32_t avail = jbuf_available(jbuf);

        /* Wait for RTP sync point set by drain task (latency_ms ahead of first packet) */
        if (!rtp->sync_ready) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* Mark jbuf as playing once sync is ready (keeps overflow/underflow logic happy) */
        if (jbuf->state != JBUF_PLAYING) {
            ESP_LOGI(TAG, "Sync reached, starting playback: %lu bytes in jbuf", (unsigned long)avail);
            jbuf->state = JBUF_PLAYING;
        }

        /* Try to read one complete frame */
        if (avail >= 2) {
            uint8_t hdr[2];
            jbuf_peek(jbuf, hdr, 2);
            uint16_t frame_len = (hdr[0] << 8) | hdr[1];
            uint32_t total = 2 + frame_len;

            if (frame_len == 0) {
                /* PLC marker frame: pass the 2-byte zero-length header to the
                 * Opus decoder so it generates concealment audio */
                if ((int)2 <= len) {
                    jbuf_read(jbuf, (uint8_t *)buffer, 2);
                    audio_element_update_byte_pos(self, 2);
                    return 2;
                }
            } else if (frame_len < 1500 && avail >= total && (int)total <= len) {
                jbuf_read(jbuf, (uint8_t *)buffer, total);
                audio_element_update_byte_pos(self, total);
                return total;
            }
        }

        /* No complete frame — wait for drain task to deliver (typically <20ms) */
        vTaskDelay(pdMS_TO_TICKS(2));
    }
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

    rtp->sync_initialized    = false;
    rtp->measured_sample_hz  = rtp->sample_rate;
    rtp->last_clk_update_us  = 0;
    rtp->rtp_ts_base         = 0;
    rtp->local_time_base_us  = 0;
    rtp->last_ssrc           = 0;
    rtp->rtp_seq             = 0;
    rtp->need_seq_seed       = true;
    rtp->last_overflow_log_us = 0;
    rtp->target_delay_ms      = (config->latency_ms > 0) ? (uint32_t)config->latency_ms : RTP_OPUS_STREAM_DEFAULT_LATENCY_MS;
    rtp->sync_rtp_ts_start    = 0;
    rtp->sync_ready           = false;

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

    /* Allocate packet receive buffer — internal RAM for speed (used by drain task) */
    if (opus_packet_buf == NULL) {
        opus_packet_buf = heap_caps_malloc(OPUS_PACKET_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (opus_packet_buf == NULL) {
            opus_packet_buf = malloc(OPUS_PACKET_BUF_SIZE);  /* fallback */
        }
        assert(opus_packet_buf != NULL);
    }

    /*
     * Jitter buffer in PSRAM — absorbs pipeline backpressure stalls.
     * 32KB ≈ 5+ seconds of Opus frames at typical bitrates.
     */
    jbuf_init(&rtp->jbuf, 16384);  /* 16KB in PSRAM — ~4s Opus capacity, Stage 1 reduction */

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

    /* Reset jitter buffer and RTP sync to clear old data */
    jbuffer *jbuf = &rtp->jbuf;
    memset(jbuf->buf, 0, jbuf->bufsize);
    jbuf->r_p = 0;
    jbuf->w_p = 0;
    jbuf->state = JBUF_IDLE;
    rtp->sync_initialized   = false;
    rtp->measured_sample_hz = rtp->sample_rate;
    rtp->last_clk_update_us = 0;
    rtp->last_ssrc          = 0;
    rtp->rtp_seq            = 0;

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

/**
 * @brief  Returns the sender's measured audio clock rate in Hz.
 *         Returns nominal sample_rate until at least 30s of data collected.
 *         Pass to i2s_stream_set_clk periodically to compensate crystal drift.
 */
int32_t rtp_opus_stream_get_measured_hz(audio_element_handle_t self)
{
    if (!self) return 48000;
    rtp_opus_stream_t *rtp = (rtp_opus_stream_t *)audio_element_getdata(self);
    if (!rtp) return 48000;
    return rtp->measured_sample_hz;
}

int32_t rtp_opus_stream_get_jbuf_fill_pct(audio_element_handle_t self)
{
    if (!self) return 0;
    rtp_opus_stream_t *rtp = (rtp_opus_stream_t *)audio_element_getdata(self);
    if (!rtp || rtp->jbuf.bufsize == 0) return 0;
    return (int32_t)(jbuf_available(&rtp->jbuf) * 100 / rtp->jbuf.bufsize);
}
