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

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "OPUS_LAN";

typedef struct __tag_OPUSCONFIG{
	uint8_t 				state;
	int 				    defaultState;
	uint8_t 				volume;
	uint8_t 				channel;
	uint8_t                 group;
    uint16_t                port;
    char *                  host;
    int                     sample_rate;
    int                     bits_per_sample;
    int                     buf_size;
    
    int                    stateReport;
    int                    volumeReport;
    int                    channelReport;

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
    opusCMD_setChannel,
    opusCMD_setVolume
} opusCMD;

#define ENABLE 1
#define DISABLE 0

void _opusSetVolume_num(audio_element_handle_t i2s_stream, uint8_t vol) {
	if(vol>100){
		vol=100;
	}
	i2s_alc_volume_set(i2s_stream, -34 + (vol / 3));
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
    i2s_cfg.use_alc = true;
    i2s_cfg.task_core = 1;
    i2s_cfg.stack_in_ext = true;     /* Task stack in PSRAM to save internal RAM */
    i2s_cfg.out_rb_size = 8 * 1024;  /* Default: need room for decoded 48kHz stereo PCM */
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
    opus_cfg.task_core = 1;
    opus_cfg.out_rb_size = 8 * 1024; /* Each decoded 48kHz stereo frame = 3840 bytes, need 2+ frames */
    opus_cfg.task_stack = 30 * 1024; /* Default, stack in PSRAM */
    opus_cfg.stack_in_ext = true;   /* Put decoder task stack in PSRAM */
    c->opus_decoder = raw_opus_decoder_init(&opus_cfg);
    ESP_LOGI(TAG, "Free heap after opus decoder init: %u (internal: %u)",
             xPortGetFreeHeapSize(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    AUDIO_NULL_CHECK(TAG, c->opus_decoder, return ESP_FAIL);

    ESP_LOGI(TAG, "[2.3] Create RTP Opus stream to read data");
    if (c->host != NULL) {
        free(c->host);
        c->host = NULL;
    }
    c->host = calloc(16, sizeof(char));
    sprintf(c->host, "239.0.%d.%d", c->group, c->channel);
    rtp_opus_stream_cfg_t rtp_cfg = RTP_OPUS_STREAM_CFG_DEFAULT();
    rtp_cfg.type = AUDIO_STREAM_READER;
    rtp_cfg.port = c->port;
    rtp_cfg.host = c->host;
    rtp_cfg.task_core = 0;
    rtp_cfg.buf_size = c->buf_size;  /* Use configured buffer size */
    rtp_cfg.sample_rate = c->sample_rate;
    rtp_cfg.bits_per_sample = c->bits_per_sample;
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
    
    /* Группа
    - 239.0.Х.0 по умолчанию 7
	*/
	c->group =  get_option_int_val(slot_num, "group", "num", 7, 0, 255);
    ESP_LOGD(TAG, "[opusLAN_%d] group:%d", slot_num, c->group);

    /* Канал
    - 239.0.7.Х по умолчанию 0
	*/
	c->channel =  get_option_int_val(slot_num, "channel", "num", 0, 0, 255);
    ESP_LOGD(TAG, "[opusLAN_%d] channel:%d", slot_num, c->channel);

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

    /* Размер буфера
    - по умолчанию 2048
	*/
	c->buf_size = get_option_int_val(slot_num, "bufSize", "num", 2048, 256, 8192);
    ESP_LOGD(TAG, "[opusLAN_%d] bufSize:%d", slot_num, c->buf_size);

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
    
    /* Рапортует номер канала
	*/
	c->channelReport = stdreport_register(RPTT_int, slot_num, "num", "channel");

    /* Команда включает/выключает плеер
    */
    stdcommand_register(&c->cmds, opusCMD_setState, "setState", PARAMT_int);

    /* Команда устанавливает номер канала
    0-255
    */
    stdcommand_register(&c->cmds, opusCMD_setChannel, "setChannel", PARAMT_int);

    /* Команда устанавливает значение громкости
    0-100
    */
    stdcommand_register(&c->cmds, opusCMD_setVolume, "setVolume", PARAMT_int);
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
    if(c->state == ENABLE){
        audio_pipeline_resume(c->pipeline);
        ESP_LOGD(TAG,"opusLAN_%d state ENABLE", slot_num);
    }else{
        audio_pipeline_pause(c->pipeline);
        ESP_LOGD(TAG,"opusLAN_%d slot state DISABLE", slot_num);
    }

    stdreport_i(c->stateReport, c->state);
    stdreport_i(c->channelReport, c->channel);
    stdreport_i(c->volumeReport, c->volume);

    int64_t last_blink_time = 0;
    int led_state = 0;

    while (1) {
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

            case opusCMD_setChannel:
                {
                    char* old_host = c->host;

                    c->channel = atoi(cmd_arg);
                    c->host = calloc(16, sizeof(char));
                    sprintf(c->host, "239.0.%d.%d", c->group, c->channel);

                    esp_err_t switch_result = rtp_opus_stream_switch_multicast_address(c->rtp_stream_reader, c->host, c->port);

                    if (switch_result == ESP_OK) {
                        ESP_LOGI(TAG, "[opusLAN_%d] Successfully switched to multicast address %s", slot_num, c->host);
                        if (old_host) {
                            free(old_host);
                        }
                    } else {
                        ESP_LOGW(TAG, "[opusLAN_%d] Failed to switch, restarting pipeline", slot_num);
                        if (c->host) {
                            free(c->host);
                        }
                        c->host = old_host;
                        if (c->state == ENABLE) {
                            opusPipelineStop(c);
                            opusPipelineStart(c);
                            audio_pipeline_resume(c->pipeline);
                            ESP_LOGD(TAG, "[opusLAN_%d] stream restarted on channel:%d. Free heap:%d", slot_num, c->channel, xPortGetFreeHeapSize());
                        }
                    }

                    ESP_LOGD(TAG, "[opusLAN_%d] setChannel state:%d host:%s group:%d channel:%d Free heap:%d", slot_num, c->state, c->host, c->group, c->channel, xPortGetFreeHeapSize());
                    stdreport_i(c->channelReport, c->channel);
                }
                break;
            
            case opusCMD_setVolume:
                c->volume = atoi(cmd_arg);
                _opusSetVolume_num(c->i2s_stream_writer, c->volume);
                ESP_LOGD(TAG, "[opusLAN_%d] setVolume:%d. Free heap:%d", slot_num, c->volume, xPortGetFreeHeapSize());
                stdreport_i(c->volumeReport, c->volume);
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
