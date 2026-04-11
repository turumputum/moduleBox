/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2020 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "audio_mem.h"
#include "rtp_client_stream.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

static const char *TAG = "RTP_STREAM";
static const char *V4TAG = "mcast-v4";

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

typedef struct rtp_stream {
    //int                           hdr_init;
    audio_stream_type_t           type;
    int                           sock;
    int                           port;
    char                          *host;
    bool                          is_open;
   // int                           timeout_ms;
    rtp_stream_event_handle_cb    hook;
   // esp_transport_list_handle_t   transport_list;
    void                          *ctx;
    jbuffer                       jbuf;
    bool                          is_multicast;  // Flag to indicate if this is a multicast stream
    char                          prev_host[16]; // Store previous host for leaving multicast group
    int64_t                       last_packet_time;
    int                           sample_rate;
    int                           bits_per_sample;

    // === Clock drift correction state ===
    uint32_t                      first_rtp_ts;       // First RTP timestamp received
    int64_t                       total_samples_to_i2s; // Total samples actually returned to I2S (post-correction)
    bool                          drift_initialized;  // Whether drift tracking has started
    uint32_t                      last_rtp_ts;        // Last RTP timestamp seen
    uint16_t                      last_seq;           // Last sequence number (moved from static)
    int64_t                       last_drift_log_us;  // Last time drift was logged
    int32_t                       drift_correction_total; // Net corrections (+ = inserted, - = dropped)

    // Rate-based drift detection: measure slope of raw_drift over windows
    int64_t                       drift_start_us;        // Time when drift tracking started
    int32_t                       drift_window_raw;      // raw_drift snapshot at window start
    int64_t                       drift_window_start_us; // Time of window start snapshot
    bool                          drift_window_valid;    // Whether we have a valid window start
    int32_t                       drift_rate_mpps;       // Measured drift rate in milli-samples/sec (x1000)
    int64_t                       last_correction_us;    // Rate limiter for corrections
    int32_t                       correction_debt;       // Accumulated fractional corrections needed
} rtp_stream_t;

struct rtp_header {
    unsigned int version:2;     /* protocol version */
    unsigned int padding:1;     /* padding flag */
    unsigned int extension:1;   /* header extension flag */
    unsigned int cc:4;          /* CSRC count */
    unsigned int marker:1;      /* marker bit */
    unsigned int pt:7;          /* payload type */
    uint16_t seq:16;            /* sequence number */
    uint32_t ts;                /* timestamp */
    uint32_t ssrc;              /* synchronization source */
    uint32_t csrc[1];           /* optional CSRC list */
};

// // ///////////////////////////////////////////////////////////////////////////////////////////////////////
static int socket_add_ipv4_multicast_group(int sock, bool assign_source_if, char* host)
{
    struct ip_mreq imreq = { 0 };
    struct in_addr iaddr = { 0 };
    int err = 0;

    // listen for all interfaces
    imreq.imr_interface.s_addr = IPADDR_ANY;

/*
    esp_netif_ip_info_t ip_info = { 0 };
    err = esp_netif_get_ip_info(get_example_netif(), &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(V4TAG, "Failed to get IP address info. Error 0x%x", err);
        goto err;
    }
    inet_addr_from_ip4addr(&iaddr, &ip_info.ip);
*/

    // Configure multicast address to listen to
    err = inet_aton(host, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(V4TAG, "Configured IPV4 multicast address '%s' is invalid.", host);
        // Errors in the return value have to be negative
        err = -1;
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));
    if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
        ESP_LOGW(V4TAG, "Configured IPV4 multicast address '%s' is not a valid multicast address. This will probably not work.", host);
    }

    if (assign_source_if) {
        // Assign the IPv4 multicast source interface, via its IP
        // (only necessary if this socket is IPV4 only)
        err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iaddr,
                         sizeof(struct in_addr));
        if (err < 0) {
            ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_IF. Error %d", errno);
            goto err;
        }
    }

    err = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_ADD_MEMBERSHIP. Error %d", errno);
        goto err;
    }

 err:
    return err;
}

// Function to leave multicast group for a specific address
static int socket_leave_ipv4_multicast_group(int sock, char* host)
{
    struct ip_mreq imreq = { 0 };
    int err = 0;

    // Validate socket is valid
    if (sock < 0) {
        ESP_LOGW(V4TAG, "Invalid socket %d for leaving multicast group", sock);
        return -1;
    }

    // Configure multicast address to leave
    err = inet_aton(host, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(V4TAG, "Configured IPV4 multicast address '%s' is invalid for leaving.", host);
        err = -1;
        goto err;
    }

    // Use any interface to leave the group
    imreq.imr_interface.s_addr = IPADDR_ANY;

    err = setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                         &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_DROP_MEMBERSHIP for %s. Error %d", host, errno);
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
        ESP_LOGE(V4TAG, "Failed to create socket. Error %d", errno);
        return -1;
    }

    // Increase socket receive buffer to reduce packet loss
    int rcvbuf_size = 16384;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));

    // Bind the socket to any address
    saddr.sin_family = PF_INET;
    saddr.sin_port = htons(port);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to bind socket. Error %d", errno);
        goto err;
    }


    // Assign multicast TTL (set separately from normal interface TTL)
    uint8_t ttl = MULTICAST_TTL;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_TTL. Error %d", errno);
        goto err;
    }

#if MULTICAST_LOOPBACK
    // select whether multicast traffic should be received by this device, too
    // (if setsockopt() is not called, the default is no)
    uint8_t loopback_val = MULTICAST_LOOPBACK;
    err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP,
                     &loopback_val, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_LOOP. Error %d", errno);
        goto err;
    }
#endif

    // this is also a listening socket, so add it to the multicast
    // group for listening...
    err = socket_add_ipv4_multicast_group(sock, true, host);
    if (err < 0) {
        goto err;
    }

    // All set, socket is configured for sending and receiving
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

    // Increase socket receive buffer to reduce packet loss
    int rcvbuf_size = 16384;
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

// Function to switch to a new multicast group without closing the socket
static int socket_switch_multicast_group(int sock, char* old_host, char* new_host)
{
    int err = 0;

    // Validate socket is valid
    if (sock < 0) {
        ESP_LOGE(TAG, "Invalid socket %d for multicast group switch", sock);
        return -1;
    }

    // Leave the old multicast group if we were part of one
    if (old_host != NULL && strlen(old_host) > 0 && strcmp(old_host, new_host) != 0) {
        err = socket_leave_ipv4_multicast_group(sock, old_host);
        if (err < 0) {
            ESP_LOGW(TAG, "Warning: Failed to leave old multicast group %s, continuing...", old_host);
            // Continue anyway as we might still be able to join the new group
        }
    }

    // Join the new multicast group
    err = socket_add_ipv4_multicast_group(sock, true, new_host);
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to join new multicast group %s", new_host);
        return err;
    }

    ESP_LOGI(TAG, "Successfully switched from multicast group %s to %s",
             old_host ? old_host : "(none)", new_host);

    return 0;
}

// // ///////////////////////////////////////////////////////////////////////////////////////////////////////

static int jbuf_free(jbuffer *jbuf)
{
    free (jbuf->buf);
    return 0;
}

static int jbuf_init(jbuffer *jbuf, uint32_t size)
{
    jbuf->buf = malloc(size);
    assert(jbuf->buf != NULL);

    memset (jbuf->buf, 0, size);
    jbuf->bufsize = size;
    jbuf->r_p = 0;
    jbuf->w_p = size;
    jbuf->read_thres = (size/4)*3;
    jbuf->min_thres = size/4;
    jbuf->state = JBUF_IDLE;
    return 0;
}

// silently drops data if writing too much
static int jbuf_write(uint8_t *datain, uint32_t size, jbuffer *jbuf)
{    
    uint32_t i=0;
    while (size)
    {
        uint32_t new_w_p = jbuf->w_p + 1;

        if (new_w_p >= jbuf->bufsize) new_w_p=0;
        if (new_w_p == jbuf->r_p) break;
        
        jbuf->w_p = new_w_p; 
        jbuf->buf[jbuf->w_p] = datain[i];
        i++;
        size--;
    }

    return i;
}

/// return read amount, size - requested amount of data
static int jbuf_read(uint8_t *dataout, uint32_t size, jbuffer *jbuf)
{
    uint32_t i=0;
    while (size)
    {
        if (jbuf->r_p == jbuf->w_p) break;
        dataout[i] = jbuf->buf[jbuf->r_p];
        
        uint32_t new_r_p = jbuf->r_p + 1;

        if (new_r_p >= jbuf->bufsize) new_r_p=0;
        
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

static esp_err_t _dispatch_event(audio_element_handle_t el, rtp_stream_t *tcp, void *data, int len, rtp_stream_status_t state)
{
    if (el && tcp && tcp->hook) {
        rtp_stream_event_msg_t msg = { 0 };
        msg.data = data;
        msg.data_len = len;
        msg.sock_fd = tcp->sock;
        msg.source = el;
        return tcp->hook(&msg, state, tcp->ctx);
    }
    return ESP_FAIL;
}

static esp_err_t _rtp_open(audio_element_handle_t self)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    rtp_stream_t *rtp = (rtp_stream_t *)audio_element_getdata(self);
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
        _get_socket_error_code_reason(__func__,  rtp->sock);
        goto _exit;
    }
    rtp->is_open = true;
    rtp->last_packet_time = esp_timer_get_time();
    // Reset drift tracking on each new connection
    rtp->drift_initialized = false;
    rtp->last_seq = 0;
    _dispatch_event(self, rtp, NULL, 0, RTP_STREAM_STATE_CONNECTED);


    // struct timeval tv;
    // tv.tv_sec = 5;  // Таймаут 5 секунд на любую операцию
    // tv.tv_usec = 0;
    // setsockopt(rtp->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    // setsockopt(rtp->sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));


    return ESP_OK;

_exit:
    
    return ESP_FAIL;
}

static esp_err_t _rtp_close(audio_element_handle_t self){

    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    rtp_stream_t *rtp = (rtp_stream_t *)audio_element_getdata(self);
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

static esp_err_t _rtp_destroy(audio_element_handle_t self){
    ESP_LOGD(TAG, "_rtp_destroy");
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    rtp_stream_t *rtp = (rtp_stream_t *)audio_element_getdata(self);
    AUDIO_NULL_CHECK(TAG, rtp, return ESP_FAIL);

    jbuf_free(&rtp->jbuf);

    // Note: The host string is managed by the caller (rtp_play.c),
    // so we don't free it here to avoid double-free issues

    audio_free(rtp);
    return ESP_OK;
}

uint8_t packet_buf[1600]; //fixme todo

// Extract 32-bit RTP timestamp from packet bytes 4-7 (network byte order)
static inline uint32_t rtp_get_timestamp(const uint8_t *pkt) {
    return ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
           ((uint32_t)pkt[6] << 8)  | (uint32_t)pkt[7];
}

static esp_err_t _rtp_read(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    rtp_stream_t *rtp = (rtp_stream_t *)audio_element_getdata(self);

    struct sockaddr_in source_addr;
    socklen_t addr_len = sizeof(source_addr);
    
    int saved =0;
    int ret = 0;
    int wr = -4;

    while (1)
    {
        if (audio_element_is_stopping(self)) {
            ESP_LOGW(TAG, "_rtp_read: Received stop command, aborting.");
            _rtp_close(self);
            _rtp_destroy(self);
            return ESP_FAIL;
        }

        ret = recvfrom(rtp->sock, packet_buf, 1600, 0, (struct sockaddr *)&source_addr, &addr_len);
        if (ret>0) 
        {
            rtp->last_packet_time = esp_timer_get_time();
            if (ret%4!=0) 
            {
                ESP_LOGE(TAG, "Wrong packet size! %d", ret-12);
                continue;
            }
            
            uint16_t tseq = packet_buf[2] << 8 | packet_buf[3];
            if ((uint16_t)tseq == (uint16_t)rtp->last_seq)  
            {
                ESP_LOGE(TAG, "Duplicated packet! %d", tseq);
                continue;
            }

            // === Extract RTP timestamp and track clock drift ===
            uint32_t rtp_ts = rtp_get_timestamp(packet_buf);
            int64_t now_us = esp_timer_get_time();

            if (!rtp->drift_initialized) {
                // First packet — start drift tracking
                rtp->first_rtp_ts = rtp_ts;
                rtp->last_rtp_ts = rtp_ts;
                rtp->total_samples_to_i2s = 0;
                rtp->drift_correction_total = 0;
                rtp->drift_start_us = now_us;
                rtp->drift_window_valid = false;
                rtp->drift_window_raw = 0;
                rtp->drift_window_start_us = 0;
                rtp->drift_rate_mpps = 0;
                rtp->last_drift_log_us = now_us;
                rtp->last_correction_us = now_us;
                rtp->correction_debt = 0;
                rtp->drift_initialized = true;
                ESP_LOGI(TAG, "[drift] Initialized: rtp_ts=%lu sample_rate=%d",
                         (unsigned long)rtp_ts, rtp->sample_rate);
            } else {
                rtp->last_rtp_ts = rtp_ts;
            }

            for (uint16_t i=12; i< ret-1; i=i+2)
            {
                uint8_t t=packet_buf[i];
                packet_buf[i]=packet_buf[i+1];
                packet_buf[i+1]=t;
            }
            
            saved = jbuf_write(packet_buf+12, ret-12, &rtp->jbuf);
            
            if ((uint16_t)tseq != (uint16_t)(rtp->last_seq + 1))
                ESP_LOGE(TAG, "Missing packet! expected %d got %d", rtp->last_seq + 1, tseq);
            rtp->last_seq = tseq; 
        }else if (ret < 0) 
        {
            // === ИСПРАВЛЕНИЕ 2: Обработка ошибок сокета/закрытия ===
            // Проверяем код ошибки errno. EWOULDBLOCK/EAGAIN ожидаемы при использовании таймаутов/неблокирующего режима.
            // Если сокет был закрыт извне (_rtp_close), recvfrom может вернуть другую ошибку (например, EBADF).
            if (errno == EWOULDBLOCK || errno == EAGAIN || audio_element_is_stopping(self)) {
                 vTaskDelay(pdMS_TO_TICKS(10)); // Небольшая задержка, чтобы не нагружать CPU в цикле ожидания
                 continue; // Продолжаем ждать данные/сигнал останова
            } else {
                 ESP_LOGE(TAG, "_rtp_read: recvfrom error: %d (%s)", errno, strerror(errno));
                 return ESP_FAIL; // Критическая ошибка, можно вернуть FAIL или ABORT
            }
        }


        switch (rtp->jbuf.state)
        {
            case JBUF_IDLE:
                rtp->jbuf.state = JBUF_FILLING;
                //break;
            case JBUF_FILLING:
                if (rtp->jbuf.read_thres > jbuf_count(&rtp->jbuf)) 
                {
                    if (ret == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) vTaskDelay(1);
                    continue;
                }
                //jbuf is full
                rtp->jbuf.state = JBUF_PLAYING;
                break;
            case JBUF_PLAYING:
                if (rtp->jbuf.min_thres > jbuf_count(&rtp->jbuf)) 
                {
                    rtp->jbuf.state = JBUF_FILLING;
                    continue;
                }
                break;
        }
        
        break;
    }
    int available = jbuf_count(&rtp->jbuf);
    unsigned int r,w;
    r=rtp->jbuf.r_p;
    w=rtp->jbuf.w_p;
    int readlen = available >4? (available > len ? len : (available/4-1)*4) : 0; //workaround shitty circbuf

    wr = jbuf_read((uint8_t *)buffer, readlen, &rtp->jbuf);
    
    // === Clock drift correction via drift RATE measurement ===
    //
    // We track raw_drift = (total_samples_to_i2s - rtp_elapsed).
    // Its absolute value includes a large constant offset (jitter buffer fill,
    // pipeline latency) — this is NOT drift.
    // Only the SLOPE (rate of change) of raw_drift indicates real clock drift.
    //
    // Algorithm:
    //   1. SETTLE period (15s): let pipeline fill stabilize, do nothing
    //   2. Start measurement window, record raw_drift snapshot
    //   3. Every WINDOW_SEC (30s): compute slope = delta_raw / delta_time
    //   4. If |slope| > MIN_RATE: accumulate correction_debt at that rate
    //   5. When |correction_debt| >= 1 sample: apply insertion/drop
    //
    // Typical crystal drift ~20-50 ppm → 0.96-2.4 samples/sec at 48kHz
    // Over 30s window: 29-72 samples → clearly measurable
    //
    #define DRIFT_SETTLE_US         (15 * 1000000LL)   // 15s settle before measuring
    #define DRIFT_WINDOW_US         (30 * 1000000LL)   // 30s measurement window
    #define DRIFT_MIN_RATE_MPPS     300                // min drift rate to act on: 0.3 samples/sec (in milli-samples/sec)
    #define DRIFT_CORRECTION_COOLDOWN_US (100000LL)    // min 100ms between corrections

    if (rtp->drift_initialized && wr > 0 && rtp->sample_rate > 0) {
        int bytes_per_frame = (rtp->bits_per_sample / 8) * 2; // stereo frame size
        int frames_this_read = wr / bytes_per_frame;
        rtp->total_samples_to_i2s += frames_this_read;

        // RTP elapsed samples (sender's clock)
        uint32_t rtp_elapsed = rtp->last_rtp_ts - rtp->first_rtp_ts;
        // Raw drift = our consumption minus sender's timeline (constant offset + accumulated drift)
        int32_t raw_drift = (int32_t)(rtp->total_samples_to_i2s - (int64_t)rtp_elapsed);

        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_us = now_us - rtp->drift_start_us;

        // Phase 1: Settle — wait for pipeline to stabilize
        if (!rtp->drift_window_valid && elapsed_us >= DRIFT_SETTLE_US) {
            // Take first snapshot to start measurement window
            rtp->drift_window_raw = raw_drift;
            rtp->drift_window_start_us = now_us;
            rtp->drift_window_valid = true;
            rtp->drift_rate_mpps = 0;
            rtp->correction_debt = 0;
            ESP_LOGI(TAG, "[drift] Window started: raw=%ld", (long)raw_drift);
        }

        // Phase 2: Measure drift rate and apply corrections
        if (rtp->drift_window_valid) {
            int64_t window_elapsed_us = now_us - rtp->drift_window_start_us;

            // Re-measure drift rate every WINDOW_SEC
            if (window_elapsed_us >= DRIFT_WINDOW_US) {
                int32_t delta_raw = raw_drift - rtp->drift_window_raw;
                // drift_rate in milli-samples per second (x1000 for precision)
                // delta_raw is in samples, window_elapsed_us is in microseconds
                rtp->drift_rate_mpps = (int32_t)((int64_t)delta_raw * 1000000000LL / window_elapsed_us);

                ESP_LOGI(TAG, "[drift] Window: delta_raw=%ld over %.1fs → rate=%ld mpps (%.2f samp/s)",
                         (long)delta_raw,
                         (float)window_elapsed_us / 1000000.0f,
                         (long)rtp->drift_rate_mpps,
                         (float)rtp->drift_rate_mpps / 1000.0f);

                // Slide window forward
                rtp->drift_window_raw = raw_drift;
                rtp->drift_window_start_us = now_us;

                // If rate is below noise threshold, zero out debt
                if (abs(rtp->drift_rate_mpps) < DRIFT_MIN_RATE_MPPS) {
                    rtp->correction_debt = 0;
                }
            }

            // Accumulate correction debt based on measured rate
            // debt accumulates in milli-samples; correct when |debt| >= 1000 (= 1 sample)
            if (abs(rtp->drift_rate_mpps) >= DRIFT_MIN_RATE_MPPS) {
                // Calculate how many milli-samples of debt to add for this read
                // frames_this_read samples at sample_rate → time = frames / rate seconds
                // debt_delta = drift_rate_mpps * (frames / rate) milli-samples
                int32_t debt_delta = (int32_t)((int64_t)rtp->drift_rate_mpps * frames_this_read / rtp->sample_rate);
                rtp->correction_debt += debt_delta;
            }

            // Apply corrections when debt reaches 1 full sample (1000 milli-samples)
            bool cooldown_ok = (now_us - rtp->last_correction_us) > DRIFT_CORRECTION_COOLDOWN_US;

            if (cooldown_ok && wr >= bytes_per_frame * 4) {
                if (rtp->correction_debt >= 1000) {
                    // Positive rate: local clock FAST, consuming more → DROP one frame
                    int drop_pos = (wr / 2 / bytes_per_frame) * bytes_per_frame;
                    if (drop_pos + bytes_per_frame <= wr) {
                        memmove(buffer + drop_pos,
                                buffer + drop_pos + bytes_per_frame,
                                wr - drop_pos - bytes_per_frame);
                        wr -= bytes_per_frame;
                        rtp->drift_correction_total--;
                        rtp->correction_debt -= 1000;
                        rtp->last_correction_us = now_us;
                    }
                } else if (rtp->correction_debt <= -1000) {
                    // Negative rate: local clock SLOW, consuming less → INSERT one frame
                    if (wr + bytes_per_frame <= len) {
                        int dup_pos = (wr / 2 / bytes_per_frame) * bytes_per_frame;
                        memmove(buffer + dup_pos + bytes_per_frame,
                                buffer + dup_pos,
                                wr - dup_pos);
                        wr += bytes_per_frame;
                        rtp->drift_correction_total++;
                        rtp->correction_debt += 1000;
                        rtp->last_correction_us = now_us;
                    }
                }
            }
        }

        // Log drift statistics every 10 seconds
        if ((now_us - rtp->last_drift_log_us) > 10000000) {
            int jbuf_fill = jbuf_count(&rtp->jbuf);
            ESP_LOGI(TAG, "[drift] rtp_el=%lu i2s=%lld raw=%ld rate=%ld.%03ld samp/s debt=%ld corr=%ld jbuf=%d/%d %s",
                     (unsigned long)rtp_elapsed,
                     rtp->total_samples_to_i2s,
                     (long)raw_drift,
                     (long)(rtp->drift_rate_mpps / 1000),
                     (long)(abs(rtp->drift_rate_mpps) % 1000),
                     (long)rtp->correction_debt,
                     (long)rtp->drift_correction_total,
                     jbuf_fill, (int)rtp->jbuf.bufsize,
                     rtp->drift_window_valid ? "[active]" : "[settling]");
            rtp->last_drift_log_us = now_us;
        }
    }

    audio_element_update_byte_pos(self, wr);
    if (wr%4!=0) ESP_LOGE(TAG, "wrong count written %d requested %d available %d %d %d", wr, len, available, r, w); 

    return wr > 0 ? wr : -4;
}

static esp_err_t _rtp_write(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    return ESP_FAIL;
}

static esp_err_t _rtp_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    int r_size = audio_element_input(self, in_buffer, in_len);
    int w_size = 0;
    if (r_size > 0) {
        w_size = audio_element_output(self, in_buffer, r_size);
        if (w_size > 0) {
            audio_element_update_byte_pos(self, r_size);
        }
    } else {
        w_size = r_size;
    }
    return w_size;
}



audio_element_handle_t rtp_stream_init(rtp_stream_cfg_t *config)
{
    AUDIO_NULL_CHECK(TAG, config, return NULL);

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    audio_element_handle_t el;
    cfg.open = _rtp_open;
    cfg.close = _rtp_close;
    cfg.process = _rtp_process;
    cfg.destroy = _rtp_destroy;
    cfg.task_stack = config->task_stack;
    cfg.task_prio = config->task_prio;
    cfg.task_core = config->task_core;
    cfg.stack_in_ext = config->ext_stack;
    cfg.tag = "rtp_client";
    cfg.buffer_len = (config->buf_size > 0) ? config->buf_size : RTP_STREAM_BUF_SIZE;

    rtp_stream_t *rtp = audio_calloc(1, sizeof(rtp_stream_t));
    AUDIO_MEM_CHECK(TAG, rtp, return NULL);

    rtp->type = config->type;
    rtp->port = config->port;
    rtp->host = config->host;
    rtp->sample_rate = (config->sample_rate > 0) ? config->sample_rate : RTP_STREAM_DEFAULT_SAMPLE_RATE;
    rtp->bits_per_sample = (config->bits_per_sample > 0) ? config->bits_per_sample : RTP_STREAM_DEFAULT_BITS_PER_SAMPLE;
    rtp->is_multicast = (config->host != NULL && strlen(config->host) > 0) ? true : false;  // Check if this is a multicast address
    if (config->host) {
        strncpy(rtp->prev_host, config->host, sizeof(rtp->prev_host) - 1);
        rtp->prev_host[sizeof(rtp->prev_host) - 1] = '\0';
    } else {
        rtp->prev_host[0] = '\0';
    }
    //rtp->timeout_ms = config->timeout_ms;
    if (config->event_handler) {
        rtp->hook = config->event_handler;
        if (config->event_ctx) {
            rtp->ctx = config->event_ctx;
        }
    }

    if (config->type == AUDIO_STREAM_WRITER) {
       // cfg.write = _rtp_write;
    } else {
        cfg.read = _rtp_read;
    }

    el = audio_element_init(&cfg);
    AUDIO_MEM_CHECK(TAG, el, goto _rtp_init_exit);
    audio_element_setdata(el, rtp);

    jbuf_init(&rtp->jbuf, 4096);

    return el;
_rtp_init_exit:
    audio_free(rtp);
    return NULL;
}

// Public function to switch to a new multicast address without restarting the device
esp_err_t rtp_stream_switch_multicast_address(audio_element_handle_t self, const char* new_host, uint16_t new_port)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    rtp_stream_t *rtp = (rtp_stream_t *)audio_element_getdata(self);
    AUDIO_NULL_CHECK(TAG, rtp, return ESP_FAIL);

    if (!rtp->is_open) {
        ESP_LOGE(TAG, "RTP stream is not open, cannot switch multicast address");
        return ESP_FAIL;
    }

    if (!rtp->is_multicast) {
        ESP_LOGE(TAG, "Current stream is not multicast, cannot switch multicast address");
        return ESP_FAIL;
    }

    // Validate that the socket is valid
    if (rtp->sock < 0) {
        ESP_LOGE(TAG, "RTP stream socket is invalid, cannot switch multicast address");
        return ESP_FAIL;
    }

    // Store the current host for leaving the multicast group
    char old_host[16];
    if (rtp->host != NULL) {
        strncpy(old_host, rtp->host, sizeof(old_host) - 1);
        old_host[sizeof(old_host) - 1] = '\0';
    } else {
        old_host[0] = '\0';  // Empty string if no host
    }

    // Update the host in the rtp structure
    // Store the old host in prev_host for leaving the multicast group
    strncpy(rtp->prev_host, old_host, sizeof(rtp->prev_host) - 1);
    rtp->prev_host[sizeof(rtp->prev_host) - 1] = '\0';

    // Update the host pointer - note: caller must ensure new_host remains valid during use
    rtp->host = (char*)new_host;

    // If port changed, we need to handle that separately (for now we'll just update the port)
    if (new_port != 0 && rtp->port != new_port) {
        rtp->port = new_port;
        ESP_LOGW(TAG, "Port changed but socket reconfiguration for port changes not implemented");
    }

    // Switch to the new multicast group on the same socket
    int result = socket_switch_multicast_group(rtp->sock, old_host, (char*)new_host);
    if (result < 0) {
        ESP_LOGE(TAG, "Failed to switch multicast group from %s to %s", old_host, new_host);
        return ESP_FAIL;
    }

    // Reset the buffer to clear any old data
    jbuffer *jbuf = &rtp->jbuf;
    memset(jbuf->buf, 0, jbuf->bufsize);
    jbuf->r_p = 0;
    jbuf->w_p = jbuf->bufsize;
    jbuf->state = JBUF_IDLE;

    // Reset drift tracking for new stream
    rtp->drift_initialized = false;

    ESP_LOGI(TAG, "Successfully switched to new multicast address %s", new_host);
    return ESP_OK;
}

esp_err_t rtp_stream_check_connection(audio_element_handle_t self, int timeout_ms)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);
    rtp_stream_t *rtp = (rtp_stream_t *)audio_element_getdata(self);
    AUDIO_NULL_CHECK(TAG, rtp, return ESP_FAIL);

    if (!rtp->is_open) {
        return ESP_FAIL;
    }

    int64_t current_time = esp_timer_get_time();
    if ((current_time - rtp->last_packet_time) > (int64_t)timeout_ms * 1000) {
        return ESP_FAIL; // Timeout
    }
    return ESP_OK; // Connection active
}
