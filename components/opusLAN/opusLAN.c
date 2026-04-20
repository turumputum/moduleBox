#include <stdio.h>
#include "opusLAN.h"

#include "sdkconfig.h"
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/portmacro.h"
#include "esp_netif.h"
#include "reporter.h"
#include "stateConfig.h"
#include "board.h"
#include "esp_peripherals.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "i2s_stream.h"
#include "filter_resample.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "raw_stream.h"
#include "rtp_opus_stream.h"
#include "raw_opus_decoder.h"
#include "me_slot_config.h"

#include "esp_heap_caps.h"
#include <stdcommand.h>
#include <stdreport.h>

#include <manifest.h>
#include <mbdebug.h>

#include <generated_files/gen_opusLAN.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern uint8_t led_segment;
extern uint8_t ab_ver;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "OPUS_LAN";

// Helper: get device IP address string for reports
static const char* _get_device_ip(void) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!netif) netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        static esp_netif_ip_info_t ip_info;
        static char ip_str[16];
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            return ip_str;
        }
    }
    return "0.0.0.0";
}

typedef struct __tag_OPUSCONFIG{
	uint8_t 				state;
	int 				    defaultState;
	uint8_t 				volume;
    uint16_t                port;
    char *                  host;
    char *                  multicastAddress;  /* NULL = unicast, otherwise multicast IPv4 */
    bool                    useMulticast;
    int                     sample_rate;
    int                     bits_per_sample;
    int                     jbuf_ms;         /* jitter buffer target in ms */
    int                     latency_ms;      /* target latency / sync delay in ms (40-500, default 240) */
    
    int                    stateReport;
    int                    volumeReport;
    int                    addressReport;

    STDCOMMANDS             cmds;

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t  rtp_stream_reader;
    audio_element_handle_t  opus_decoder;
    audio_element_handle_t  i2s_stream_writer;
    audio_board_handle_t    board_handle;
    audio_event_iface_handle_t evt;

} OPUSCONFIG, * POPUSCONFIG;

typedef enum
{
	opusCMD_setState= 0,
    opusCMD_setVolume,
    opusCMD_setMulticastAddress
} opusCMD;

#define ENABLE 1
#define DISABLE 0

void _opusSetVolume_num(audio_board_handle_t board_handle, uint8_t vol) {
	if(vol>100){
		vol=100;
	}
	audio_hal_set_volume(board_handle->audio_hal, vol);
}

void opusPipelineStop(POPUSCONFIG c){
    if (c->pipeline != NULL) {
        audio_pipeline_stop(c->pipeline);
        audio_pipeline_wait_for_stop(c->pipeline);
        audio_pipeline_terminate(c->pipeline);

        if (c->rtp_stream_reader != NULL) {
            audio_pipeline_unregister(c->pipeline, c->rtp_stream_reader);
        }
        if (c->opus_decoder != NULL) {
            audio_pipeline_unregister(c->pipeline, c->opus_decoder);
        }
        if (c->i2s_stream_writer != NULL) {
            audio_pipeline_unregister(c->pipeline, c->i2s_stream_writer);
        }

        audio_pipeline_deinit(c->pipeline);
        c->pipeline = NULL;
        c->rtp_stream_reader = NULL;
        c->opus_decoder = NULL;
        c->i2s_stream_writer = NULL;
    }

    if (c->host) {
        free(c->host);
        c->host = NULL;
    }

    if (c->evt != NULL) {
        audio_event_iface_destroy(c->evt);
        c->evt = NULL;
    }

    if (c->board_handle != NULL) {
        audio_hal_ctrl_codec(c->board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_STOP);
        audio_board_deinit(c->board_handle);
        c->board_handle = NULL;
    }
}

esp_err_t opusPipelineStart(POPUSCONFIG c) {

	ESP_LOGI(TAG, "Free heap before pipeline start: %u (internal: %u)",
	         xPortGetFreeHeapSize(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

	audio_element_state_t el_state = audio_element_get_state(c->i2s_stream_writer);
	if(el_state==AEL_STATE_RUNNING){
		opusPipelineStop(c);
	}

    ESP_LOGI(TAG, "[ 1 ] Start codec chip");
    if (c->board_handle != NULL) {
        audio_hal_ctrl_codec(c->board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_STOP);
        audio_board_deinit(c->board_handle);
        c->board_handle = NULL;
    }
#ifdef BOARD_PINOUT_V6
    ab_ver = 6;
#else
    if(me_config.boardVersion==4){
        ab_ver = 4;
    }
#endif
    c->board_handle = audio_board_init();
    audio_hal_ctrl_codec(c->board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[2.0] Create audio pipeline for Opus playback");
    if (c->pipeline != NULL) {
        opusPipelineStop(c);
    }
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    c->pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(c->pipeline);

    ESP_LOGI(TAG, "[2.1] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.use_alc = false;
    i2s_cfg.task_core = 0;
    i2s_cfg.stack_in_ext = true;     /* Task stack in PSRAM to save internal RAM */
    i2s_cfg.out_rb_size = 4096;      /* ~21ms of 48kHz stereo PCM — smaller = less desync between devices */
    i2s_cfg.buffer_len = 3600;       /* Default DMA buffer size */
    c->i2s_stream_writer = i2s_stream_init(&i2s_cfg);
    ESP_LOGI(TAG, "Free heap after i2s init: %u (internal: %u)",
             xPortGetFreeHeapSize(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    AUDIO_NULL_CHECK(TAG, c->i2s_stream_writer, return ESP_FAIL);

    ESP_LOGI(TAG, "[2.2] Create Opus decoder element");
    raw_opus_dec_cfg_t opus_cfg = RAW_OPUS_DEC_CONFIG_DEFAULT();
    opus_cfg.sample_rate = c->sample_rate;
    opus_cfg.channels = 2;
    opus_cfg.dec_frame_size = c->sample_rate / 50;  /* 20ms frame: 48000/50=960 samples */
    opus_cfg.self_delimited = false;
    opus_cfg.enable_frame_length_prefix = true;
    opus_cfg.task_core = 1;           /* Core 1: keep core 0 free for lwIP/Ethernet — Opus decoding is CPU-heavy */
    opus_cfg.out_rb_size = 4096;      /* ~1 decoded frame (3840B) — smaller = less desync between devices */
    opus_cfg.task_stack = 30 * 1024;  /* Default, stack in PSRAM */
    opus_cfg.stack_in_ext = true;     /* Put decoder task stack in PSRAM */
    c->opus_decoder = raw_opus_decoder_init(&opus_cfg);
    ESP_LOGI(TAG, "Free heap after opus decoder init: %u (internal: %u)",
             xPortGetFreeHeapSize(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    AUDIO_NULL_CHECK(TAG, c->opus_decoder, return ESP_FAIL);

    ESP_LOGI(TAG, "[2.3] Create RTP Opus stream to read data");
    if (c->host != NULL) {
        free(c->host);
        c->host = NULL;
    }
    if (c->useMulticast) {
        c->host = strdup(c->multicastAddress);
        ESP_LOGI(TAG, "Multicast mode: %s:%d", c->host, c->port);
    } else {
        c->host = NULL;
        ESP_LOGI(TAG, "Unicast mode: listening on port %d", c->port);
    }
    rtp_opus_stream_cfg_t rtp_cfg = RTP_OPUS_STREAM_CFG_DEFAULT();
    rtp_cfg.type = AUDIO_STREAM_READER;
    rtp_cfg.port = c->port;
    rtp_cfg.host = c->host;  /* NULL for unicast, multicast address for multicast */
    rtp_cfg.task_core = 1;        /* Core 1: drain task also on core 1, keeps core 0 for lwIP */
    rtp_cfg.task_prio = 23;       /* Higher than default 22 */
    rtp_cfg.buf_size = 1500;      /* audio element buffer: enough for max Opus frame */
    rtp_cfg.jbuf_ms = c->jbuf_ms;
    rtp_cfg.sample_rate = c->sample_rate;
    rtp_cfg.bits_per_sample = c->bits_per_sample;
    rtp_cfg.latency_ms = c->latency_ms;
    c->rtp_stream_reader = rtp_opus_stream_init(&rtp_cfg);
    ESP_LOGI(TAG, "Free heap after rtp init: %u (internal: %u)",
             xPortGetFreeHeapSize(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    AUDIO_NULL_CHECK(TAG, c->rtp_stream_reader, return ESP_FAIL);

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(c->pipeline, c->rtp_stream_reader, "rtp");
    audio_pipeline_register(c->pipeline, c->opus_decoder, "opus");
    audio_pipeline_register(c->pipeline, c->i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.5] Link: rtp --> opus_decoder --> i2s");
    const char *link_tag[3] = {"rtp", "opus", "i2s"};
    audio_pipeline_link(c->pipeline, link_tag, 3);
    ESP_LOGI(TAG, "Free heap after pipeline link: %u (internal: %u)",
             xPortGetFreeHeapSize(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    ESP_LOGI(TAG, "[ 4 ] Set up event listener");
    if (c->evt != NULL) {
        audio_event_iface_destroy(c->evt);
        c->evt = NULL;
    }
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    c->evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening events from pipeline");
    audio_pipeline_set_listener(c->pipeline, c->evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(c->pipeline);

    i2s_stream_set_clk(c->i2s_stream_writer, c->sample_rate, 16, 2); /* Opus always 16-bit */

	ESP_LOGI(TAG, "Free heap after pipeline start: %u (internal: %u)",
	         xPortGetFreeHeapSize(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

	return ESP_OK;
}

/* 
    Модуль звук через сеть с кодеком Opus
    Задержка порядка 80мс-100мс в зависимости от сети
    slots: 0-0
*/
void configure_opusLAN(POPUSCONFIG c, int slot_num)
{
    
    stdcommand_init(&c->cmds, slot_num);

    /* задает состояние модуля по умолчанию вкл/выкл
    по умолчанию 1 - включен
    */
    c->defaultState = get_option_int_val(slot_num, "defaultState","bool", 1,0,1);
    if(c->defaultState==0){
        ESP_LOGE(TAG, "defaultState: %d for slot:%d", c->defaultState, slot_num);
    }
    c->state = c->defaultState;

    /* Громкость
    - 0-100 по умолчанию 100
	*/
	c->volume =  get_option_int_val(slot_num, "volume", "percent", 100, 0, 100);
    ESP_LOGD(TAG, "[opusLAN_%d] volume:%d", slot_num, c->volume);

    /* Multicast адрес
    - если указан, модуль работает в режиме multicast
    - если не указан, модуль слушает unicast на порту
    - формат: IPv4 адрес для multicast группы
	*/
    if (strstr(me_config.slot_options[slot_num], "multicastAddress") != NULL) {
        char * mcast_addr = get_option_string_val(slot_num, "multicastAddress", "239.0.7.0");
        c->multicastAddress = strdup(mcast_addr);
        c->useMulticast = true;
        ESP_LOGI(TAG, "[opusLAN_%d] multicast mode: %s", slot_num, c->multicastAddress);
    } else {
        c->multicastAddress = NULL;
        c->useMulticast = false;
        ESP_LOGI(TAG, "[opusLAN_%d] unicast mode", slot_num);
    }

    /* порт
    - по умолчанию 7777
	*/
	c->port =  get_option_int_val(slot_num, "port", "num", 7777, 0, UINT16_MAX);
    ESP_LOGD(TAG, "[opusLAN_%d] port:%d", slot_num, c->port);

    /* Sample rate
    - по умолчанию 48000
	*/
	c->sample_rate = get_option_int_val(slot_num, "sampleRate", "num", 48000, 8000, 96000);
    ESP_LOGD(TAG, "[opusLAN_%d] sampleRate:%d", slot_num, c->sample_rate);

    /* Bits per sample
    - по умолчанию 16
	*/
    c->bits_per_sample = 16; /* Opus decoder always outputs 16-bit PCM, not configurable */
    ESP_LOGD(TAG, "[opusLAN_%d] bitsPerSample:%d", slot_num, c->bits_per_sample);

    /* Размер буфера (jitter buffer) в миллисекундах
    - по умолчанию 500мс
	*/
	c->jbuf_ms = get_option_int_val(slot_num, "bufSize", "num", 500, 50, 5000);
    ESP_LOGD(TAG, "[opusLAN_%d] bufSize:%d ms", slot_num, c->jbuf_ms);

    /* Target latency / sync delay in ms
    - default 240ms (synchronised across all devices with same latencyMs)
    */
	c->latency_ms = get_option_int_val(slot_num, "latencyMs", "num", 50, 40, 500);
    ESP_LOGD(TAG, "[opusLAN_%d] latencyMs:%d", slot_num, c->latency_ms);

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        /* Топик 
        */
        char * custom_topic = get_option_string_val(slot_num, "topic", "/opusLAN_0");
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
        ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/opusLAN_0")+3];
		sprintf(t_str, "%s/opusLAN_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
        me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}

    /* Рапортует состояние модуля вкл/выкл
	*/
	c->stateReport = stdreport_register(RPTT_int, slot_num, "state", "state");

    /* Рапортует при изменении громкости
	*/
	c->volumeReport = stdreport_register(RPTT_int, slot_num, "percent", "volume");

    /* Рапортует текущий адрес потока */
    c->addressReport = stdreport_register(RPTT_string, slot_num, "string", "address");

    /* Команда включает/выключает плеер
    */
    stdcommand_register(&c->cmds, opusCMD_setState, "setState", PARAMT_int);

    /* Команда устанавливает значение громкости
    0-100
    */
    stdcommand_register(&c->cmds, opusCMD_setVolume, "setVolume", PARAMT_int);

    /* Команда переключает multicast адрес на лету
    - строка IPv4 адреса для multicast группы
    - 0 — переключиться на unicast
    */
    stdcommand_register(&c->cmds, opusCMD_setMulticastAddress, "setMulticastAddress", PARAMT_string);
}

void opusLAN_task(void *arg){
	int slot_num = *(int *)arg;
    if(slot_num!=0){
        char tmpStr[100];
		sprintf(tmpStr, "Wrong slot, only for SLOT_0, task terminated");
		ESP_LOGE(TAG, "%s", tmpStr);
		mblog(0, tmpStr);
		vTaskDelete(NULL);
    }

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
    vTaskDelay(pdMS_TO_TICKS(1000));
    while (me_state.LAN_init_res != ESP_OK){
		vTaskDelay(pdMS_TO_TICKS(100));
	}
    ESP_LOGI(TAG, "Lan inited, configuring opusLAN slot");

    POPUSCONFIG c = calloc(1, sizeof(OPUSCONFIG));
    STDCOMMAND_PARAMS       params = { 0 };
    char str[255];

    configure_opusLAN(c, slot_num);

    gpio_num_t led_pin = SLOTS_PIN_MAP[slot_num][3];
    esp_rom_gpio_pad_select_gpio(led_pin);
	gpio_set_direction(led_pin, GPIO_MODE_OUTPUT);
	gpio_set_level(led_pin, c->state);

    waitForWorkPermit(slot_num);
    opusPipelineStart(c);
    _opusSetVolume_num(c->board_handle, c->volume);
    if(c->state == ENABLE){
        audio_pipeline_resume(c->pipeline);
        ESP_LOGD(TAG,"opusLAN_%d state ENABLE", slot_num);
    }else{
        audio_pipeline_pause(c->pipeline);
        ESP_LOGD(TAG,"opusLAN_%d slot state DISABLE", slot_num);
    }

    stdreport_i(c->stateReport, c->state);
    stdreport_i(c->volumeReport, c->volume);
    { char _ab[48]; snprintf(_ab, sizeof(_ab), "%s:%d", c->useMulticast ? c->multicastAddress : _get_device_ip(), c->port); stdreport_s(c->addressReport, _ab); }

    int64_t last_blink_time = 0;
    int led_state = 0;
    int64_t last_clk_adjust_us = esp_timer_get_time();
    int32_t last_set_hz = c->sample_rate;
    while (1) {
        /* ----------------------------------------------------------------------- */

        //LED Logic
        if (c->state == DISABLE) {
            gpio_set_level(led_pin, 0);
        } else {
            if (rtp_opus_stream_check_connection(c->rtp_stream_reader, 500) == ESP_OK) {
                gpio_set_level(led_pin, 1);
            } else {
                if ((esp_timer_get_time() - last_blink_time) > 500000) {
                    led_state = !led_state;
                    gpio_set_level(led_pin, led_state);
                    last_blink_time = esp_timer_get_time();
                }
            }
        }
        
        /* Periodic I2S clock adjustment: match sender's actual sample rate.
         * Step by ±1 Hz max per cycle to avoid audible click from I2S reconfig.
         * Only apply when measured_hz differs from current. */
        {
            int64_t now_adj = esp_timer_get_time();
            if ((now_adj - last_clk_adjust_us) > 60000000LL && c->state == ENABLE && c->rtp_stream_reader) {
                int32_t measured_hz = rtp_opus_stream_get_measured_hz(c->rtp_stream_reader);
                if (measured_hz > 0 && measured_hz != last_set_hz) {
                    /* Clamp step to ±1 Hz for smooth transition */
                    int32_t step = measured_hz - last_set_hz;
                    if (step > 1) step = 1;
                    else if (step < -1) step = -1;
                    int32_t new_hz = last_set_hz + step;
                    i2s_stream_set_clk(c->i2s_stream_writer, new_hz, 16, 2);
                    last_set_hz = new_hz;
                    ESP_LOGI(TAG, "I2S clk adjusted to %ld Hz (target %ld)", (long)new_hz, (long)measured_hz);
                }
                last_clk_adjust_us = now_adj;
            }
        }

        int cmd = stdcommand_receive(&c->cmds, &params, 150);
		char * cmd_arg = (params.count > 0) ? params.p[0].p : (char *)"0";

        switch (cmd){
            case -1: // none
                break;

            case opusCMD_setState:
                if((atoi(cmd_arg)==ENABLE)&&(c->state != ENABLE)){
                    audio_pipeline_resume(c->pipeline);
                }else if((atoi(cmd_arg)==DISABLE)&&(c->state != DISABLE)){
                    audio_pipeline_pause(c->pipeline);
                }
                c->state = atoi(cmd_arg);
                ESP_LOGD(TAG, "[opusLAN_%d] setState:%d. Free heap:%d", slot_num, c->state, xPortGetFreeHeapSize());
                stdreport_i(c->stateReport, c->state);
                break;

            case opusCMD_setVolume:
                c->volume = atoi(cmd_arg);
                _opusSetVolume_num(c->board_handle, c->volume);
                ESP_LOGD(TAG, "[opusLAN_%d] setVolume:%d. Free heap:%d", slot_num, c->volume, xPortGetFreeHeapSize());
                stdreport_i(c->volumeReport, c->volume);
                break;

            case opusCMD_setMulticastAddress:
                {
                    bool switchToUnicast = (strcmp(cmd_arg, "0") == 0);

                    if (switchToUnicast) {
                        /* Переключение на unicast — перезапуск пайплайна без multicast адреса */
                        ESP_LOGI(TAG, "[opusLAN_%d] Switching to unicast mode", slot_num);
                        if (c->multicastAddress) {
                            free(c->multicastAddress);
                            c->multicastAddress = NULL;
                        }
                        c->useMulticast = false;
                        opusPipelineStop(c);
                        opusPipelineStart(c);
                        if (c->state == ENABLE) {
                            audio_pipeline_resume(c->pipeline);
                        } else {
                            audio_pipeline_pause(c->pipeline);
                        }
                        { char _ab[48]; snprintf(_ab, sizeof(_ab), "%s:%d", _get_device_ip(), c->port); stdreport_s(c->addressReport, _ab); }
                    } else {
                        /* Переключение на новый multicast адрес */
                        ESP_LOGI(TAG, "[opusLAN_%d] Switching to multicast: %s", slot_num, cmd_arg);

                        if (c->useMulticast && c->rtp_stream_reader) {
                            /* Уже в multicast режиме — попробовать горячее переключение */
                            char *new_addr = strdup(cmd_arg);
                            esp_err_t switch_result = rtp_opus_stream_switch_multicast_address(c->rtp_stream_reader, new_addr, c->port);

                            if (switch_result == ESP_OK) {
                                ESP_LOGI(TAG, "[opusLAN_%d] Hot-switched to %s", slot_num, new_addr);
                                if (c->multicastAddress) free(c->multicastAddress);
                                c->multicastAddress = new_addr;
                                if (c->host) free(c->host);
                                c->host = strdup(new_addr);
                                { char _ab[32]; snprintf(_ab, sizeof(_ab), "%s:%d", new_addr, c->port); stdreport_s(c->addressReport, _ab); }
                            } else {
                                ESP_LOGW(TAG, "[opusLAN_%d] Hot-switch failed, restarting pipeline", slot_num);
                                free(new_addr);
                                if (c->multicastAddress) free(c->multicastAddress);
                                c->multicastAddress = strdup(cmd_arg);
                                c->useMulticast = true;
                                opusPipelineStop(c);
                                opusPipelineStart(c);
                                if (c->state == ENABLE) {
                                    audio_pipeline_resume(c->pipeline);
                                } else {
                                    audio_pipeline_pause(c->pipeline);
                                }
                                { char _ab[32]; snprintf(_ab, sizeof(_ab), "%s:%d", c->multicastAddress, c->port); stdreport_s(c->addressReport, _ab); }
                            }
                        } else {
                            /* Был unicast или первый раз — полный перезапуск */
                            if (c->multicastAddress) free(c->multicastAddress);
                            c->multicastAddress = strdup(cmd_arg);
                            c->useMulticast = true;
                            opusPipelineStop(c);
                            opusPipelineStart(c);
                            if (c->state == ENABLE) {
                                audio_pipeline_resume(c->pipeline);
                            } else {
                                audio_pipeline_pause(c->pipeline);
                            }
                            { char _ab[32]; snprintf(_ab, sizeof(_ab), "%s:%d", c->multicastAddress, c->port); stdreport_s(c->addressReport, _ab); }
                        }
                    }
                    ESP_LOGD(TAG, "[opusLAN_%d] setMulticastAddress done. multicast:%d host:%s Free heap:%d",
                             slot_num, c->useMulticast, c->host ? c->host : "(unicast)", xPortGetFreeHeapSize());
                }
                break;
        }

        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(c->evt, &msg, 120);
        if (ret != ESP_OK) {
            continue;
        }
    }
}

void start_opusLAN_task(int slot_num){

	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
    char tmpString[60];
	sprintf(tmpString, "task_opusLAN_%d", slot_num);
	xTaskCreate(opusLAN_task, tmpString, 1024 * 10, &t_slot_num, configMAX_PRIORITIES - 20, NULL);

	ESP_LOGD(TAG, "opusLAN_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


const char * get_manifest_opusLAN()
{
	return manifesto;
}
