#include <stdio.h>
#include "rtp_play.h"

#include "sdkconfig.h"
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/portmacro.h"
// #include <driver/mcpwm.h>
#include "esp_netif.h"
#include "reporter.h"
#include "stateConfig.h"
#include "board.h"
#include "esp_peripherals.h"
#include "esp_system.h"
#include "esp_log.h"
#include "i2s_stream.h"
#include "filter_resample.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "raw_stream.h"
#include "rtp_client_stream.h"
#include "me_slot_config.h"

#include <stdcommand.h>
#include <stdreport.h>

#include <manifest.h>
#include <mbdebug.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern uint8_t led_segment;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "AUDIO_LAN";

typedef struct __tag_RTPCONFIG{
	uint8_t 				state;
	int 				    defaultState;
	uint8_t 				volume;
	uint8_t 				channel;
	uint8_t                 group;
    uint16_t                port;

    STDCOMMANDS             cmds;

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t  rtp_stream_reader, i2s_stream_writer;
    audio_board_handle_t    board_handle;
    audio_event_iface_handle_t evt;

} RTPCONFIG, * PRTPCONFIG;

typedef enum
{
	rtpCMD_enable = 0,
	rtpCMD_disable,
    rtpCMD_setChannel,
    rtpCMD_setVolume
} rtpCMD;

#define ENABLE 1
#define DISABLE 0

void _setVolume_num(audio_element_handle_t i2s_stream, uint8_t vol) {
	if(vol>100){
		vol=100;
	}
	i2s_alc_volume_set(i2s_stream, -34 + (vol / 3));
}

void pipelineStop(PRTPCONFIG c){
    audio_pipeline_stop(c->pipeline);
	audio_pipeline_wait_for_stop(c->pipeline);
	//}
	audio_pipeline_terminate(c->pipeline);
    audio_pipeline_unregister(c->pipeline, c->rtp_stream_reader);
    audio_pipeline_unregister(c->pipeline, c->i2s_stream_writer);
    audio_pipeline_deinit(c->pipeline);

}

esp_err_t pipelineStart(PRTPCONFIG c) {
	//uint32_t heapBefore = xPortGetFreeHeapSize();
	
	audio_element_state_t el_state = audio_element_get_state(c->i2s_stream_writer);
	if(el_state==AEL_STATE_RUNNING){
		pipelineStop(c);
	}

    ESP_LOGI(TAG, "[ 1 ] Start codec chip");
    c->board_handle = audio_board_init();
    audio_hal_ctrl_codec(c->board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    c->pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(c->pipeline);
    AUDIO_NULL_CHECK(TAG, c->pipeline, return ESP_FAIL);

    ESP_LOGI(TAG, "[2.1] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.use_alc = true;
    i2s_cfg.task_core=1;
    //i2s_cfg.buffer_len = 36000;
    c->i2s_stream_writer = i2s_stream_init(&i2s_cfg);
    AUDIO_NULL_CHECK(TAG, c->i2s_stream_writer, return ESP_FAIL);
    // _setVolume_num(c->i2s_stream_writer, c->volume);

    ESP_LOGI(TAG, "[2.2] Create rtp client stream to read data");
    char hostStr[15];
    sprintf(hostStr, "239.0.%d.%d", c->group, c->channel);
    rtp_stream_cfg_t rtp_cfg = RTP_STREAM_CFG_DEFAULT();
    rtp_cfg.type = AUDIO_STREAM_READER;
    rtp_cfg.port = c->port;
    //rtp_cfg.port = 7777;
    rtp_cfg.host = hostStr;
    //rtp_cfg.host = "239.0.7.1";
    rtp_cfg.task_core=1;
    c->rtp_stream_reader = rtp_stream_init(&rtp_cfg);
    AUDIO_NULL_CHECK(TAG, c->rtp_stream_reader, return ESP_FAIL);

    ESP_LOGI(TAG, "[2.3] Register all elements to audio pipeline");
    audio_pipeline_register(c->pipeline, c->rtp_stream_reader, "rtp");

    audio_pipeline_register(c->pipeline, c->i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.4] Link it together rtp-->i2s");
    audio_pipeline_link(c->pipeline, (const char *[]) {"rtp", "i2s"}, 2);

    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    c->evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(c->pipeline, c->evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(c->pipeline);

    i2s_stream_set_clk(c->i2s_stream_writer, 48000, 16, 2);

	return ESP_OK;
}

/* Модуль звук через сеть
*/
void configure_audioLAN(PRTPCONFIG c, int slot_num){
    stdcommand_init(&c->cmds, slot_num);

    /* задает значение по умолчанию
    */
    if ((c->defaultState = get_option_enum_val(slot_num, "defaultState", "disable", "enable", NULL)) < 0){
        ESP_LOGE(TAG, "defaultState: unricognized value");
    }
    c->state = c->defaultState;

    /* Громкость
    - 0-100
	*/
	c->volume =  get_option_int_val(slot_num, "volume", "", 100, 0, 100);
    ESP_LOGD(TAG, "[LANplayer_%d] volume:%d", slot_num, c->volume);
    
    /* Группа
    - 239.0.Х.0
	*/
	c->group =  get_option_int_val(slot_num, "group", "", 7, 0, 255);
    ESP_LOGD(TAG, "[LANplayer_%d] group:%d", slot_num, c->group);

    /* Канал
    - 239.0.7.Х
	*/
	c->channel =  get_option_int_val(slot_num, "channel", "", 0, 0, 255);
    ESP_LOGD(TAG, "[LANplayer_%d] channel:%d", slot_num, c->channel);

    /* порт
	*/
	c->port =  get_option_int_val(slot_num, "port", "", 7777, 0, UINT16_MAX);
    ESP_LOGD(TAG, "[LANplayer_%d] port:%d", slot_num, c->port);

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        /* Топик 
        */
        char * custom_topic = get_option_string_val(slot_num, "topic", "/audioLAN_0");
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
        ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/audioLAN_0")+3];
		sprintf(t_str, "%s/audioLAN_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
        me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}


    /* Команда включает/выключает плеер
    */
    stdcommand_register(&c->cmds, rtpCMD_enable, "enable", PARAMT_int);

    /* Команда устанавливает номер канала
    0-255
    */
    stdcommand_register(&c->cmds, rtpCMD_setChannel, "setChannel", PARAMT_int);

    /* Команда устанавливает значение громкости
    0-100
    */
    stdcommand_register(&c->cmds, rtpCMD_setVolume, "setVolume", PARAMT_int);
}

void audioLAN_task(void *arg){
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
    ESP_LOGI(TAG, "Lan inited lets configure the slot");

    PRTPCONFIG c = calloc(1, sizeof(RTPCONFIG));
    STDCOMMAND_PARAMS       params = { 0 };
    char str[255];
	
    configure_audioLAN(c, slot_num);

    gpio_num_t led_pin = SLOTS_PIN_MAP[slot_num][3];
    esp_rom_gpio_pad_select_gpio(led_pin);
	gpio_set_direction(led_pin, GPIO_MODE_OUTPUT);
	gpio_set_level(led_pin, c->state);

    waitForWorkPermit(slot_num);
    if(c->state == ENABLE){
        pipelineStart(c);
        ESP_LOGD(TAG,"audioLAN_%d state ENABLE", slot_num);
    }else{
        ESP_LOGD(TAG,"audioLAN_%d slot state DISABLE", slot_num);
    }
    //TickType_t lastWakeTime = xTaskGetTickCount();

    while (1) {

        int cmd = stdcommand_receive(&c->cmds, &params, portMAX_DELAY);
		char * cmd_arg = (params.count > 0) ? params.p[0].p : (char *)"0";
        
        switch (cmd){
            case -1: // none
                break;

            case rtpCMD_enable:
                if((atoi(cmd_arg)==ENABLE)&&(c->state != ENABLE)){
                    pipelineStart(c);
                    gpio_set_level(led_pin, !c->state);
                }else if((atoi(cmd_arg)==DISABLE)&&(c->state != DISABLE)){
                    pipelineStart(c);
                    gpio_set_level(led_pin, !c->state);
                }
                c->state = atoi(cmd_arg);
                ESP_LOGD(TAG, "[audioLAN_%d] lets set state:%d. Free heap:%d", slot_num, c->state, xPortGetFreeHeapSize());
                break;

            case rtpCMD_setChannel:
                if(c->state == ENABLE){
                    pipelineStop(c);
                    c->channel = atoi(cmd_arg);
                    pipelineStart(c);
                    ESP_LOGD(TAG, "[audioLAN_%d] stream restarted on channel:%d. Free heap:%d", slot_num, c->channel, xPortGetFreeHeapSize());
                }else if(c->state==DISABLE){
                    c->channel = atoi(cmd_arg);
                }
                
                break;
            
            case rtpCMD_setVolume:
                c->volume = atoi(cmd_arg);
                _setVolume_num(c->i2s_stream_writer, c->volume);
                ESP_LOGD(TAG, "[audioLAN_%d] setVolume:%d. Free heap:%d", slot_num, c->volume, xPortGetFreeHeapSize());
                break;
        }

        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(c->evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        // /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        // if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
        //     && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
        //     && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
        //     ESP_LOGW(TAG, "[ * ] Stop event received");
        //  //   break;
        // }
    }

}

void start_audioLAN_task(int slot_num){

	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
    char tmpString[60];
	sprintf(tmpString, "task_audioLAN_%d", slot_num);
	//xTaskCreatePinnedToCore(button_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES-5, NULL, 0);
	xTaskCreate(audioLAN_task, tmpString, 1024 * 4, &t_slot_num, 1, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "audioLAN_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

