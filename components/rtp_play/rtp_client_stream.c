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

#include "esp_log.h"
#include "esp_err.h"

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
        _get_socket_error_code_reason(__func__,  rtp->sock);
        goto _exit;
    }
    rtp->is_open = true;
    _dispatch_event(self, rtp, NULL, 0, RTP_STREAM_STATE_CONNECTED);
    return ESP_OK;

_exit:
    
    return ESP_FAIL;
}

uint8_t packet_buf[1600];
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
        ret = recvfrom(rtp->sock, packet_buf, 1600, 0, (struct sockaddr *)&source_addr, &addr_len);
        //TODO validate rtp packet here;
        static uint16_t seq=0; //FIXME must be in rtp struct
        if (ret>0) 
        {
            if (ret%4!=0) 
            {
                ESP_LOGE(TAG, "Wrong packet size! %d", ret-12);
                continue;
            }
            
            uint16_t tseq = packet_buf[2] << 8 | packet_buf[3];
            if ((uint16_t)tseq ==((uint16_t)seq))  
            {
                ESP_LOGE(TAG, "Duplicated packet! %d", tseq);
                continue;
            }

            for (uint16_t i=12; i< ret-1; i=i+2)
            {
                uint8_t t=packet_buf[i];
                packet_buf[i]=packet_buf[i+1];
                packet_buf[i+1]=t;
            }
            
            saved = jbuf_write(packet_buf+12, ret-12, &rtp->jbuf);
            //ESP_LOGE(TAG, "r %d s %d jb %d", ret, saved, jbuf_count(&rtp->jbuf));
            
            if ((uint16_t)tseq !=((uint16_t)seq+1))  ESP_LOGE(TAG, "Missing packet! expected %d got %d", seq+1, tseq);
            seq=tseq; 
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
    
    audio_element_update_byte_pos(self, wr);
    if (wr%4!=0) ESP_LOGE(TAG, "wrong count written %d requested %d available %d %d %d", wr, len, available, r, w); 
    //ESP_LOGE(TAG, "get %d saved %d returned %d jbuf %d", ret, saved, wr, jbuf_count(&rtp->jbuf));

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

static esp_err_t _rtp_close(audio_element_handle_t self)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    rtp_stream_t *rtp = (rtp_stream_t *)audio_element_getdata(self);
    AUDIO_NULL_CHECK(TAG, rtp, return ESP_FAIL);
    if (!rtp->is_open) {
        ESP_LOGE(TAG, "Already closed");
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "Shutting down socket");
    shutdown(rtp->sock, 0);
    close(rtp->sock);
    
    rtp->is_open = false;
    if (AEL_STATE_PAUSED != audio_element_get_state(self)) {
        audio_element_set_byte_pos(self, 0);
    }
    return ESP_OK;
}

static esp_err_t _rtp_destroy(audio_element_handle_t self)
{
    AUDIO_NULL_CHECK(TAG, self, return ESP_FAIL);

    rtp_stream_t *rtp = (rtp_stream_t *)audio_element_getdata(self);
    AUDIO_NULL_CHECK(TAG, rtp, return ESP_FAIL);
  
    jbuf_free(&rtp->jbuf);

    audio_free(rtp);
    return ESP_OK;
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
    if (cfg.buffer_len == 0) {
        cfg.buffer_len = RTP_STREAM_BUF_SIZE;
    }

    rtp_stream_t *rtp = audio_calloc(1, sizeof(rtp_stream_t));
    AUDIO_MEM_CHECK(TAG, rtp, return NULL);

    rtp->type = config->type;
    rtp->port = config->port;
    rtp->host = config->host;
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
