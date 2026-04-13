#include <stdio.h>
#include "audioLAN.h"

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
#include "esp_timer.h"
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

#if CONFIG_LWIP_STATS
#include "lwip/stats.h"
#endif

#include <manifest.h>
#include <mbdebug.h>

#include <generated_files/gen_audioLAN.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern uint8_t led_segment;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "AUDIO_LAN";

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

typedef struct __tag_RTPCONFIG{
	uint8_t 				state;
	int 				    defaultState;
	uint8_t 				volume;
    uint16_t                port;
    char *                  host;
    char *                  multicastAddress;  /* NULL = unicast, otherwise multicast IPv4 */
    bool                    useMulticast;
    int                     sample_rate;
    int                     bits_per_sample;
    int                     jbuf_ms;
    
    int                    stateReport;
    int                    volumeReport;
    int                    addressReport;

    STDCOMMANDS             cmds;

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t  rtp_stream_reader, i2s_stream_writer;
    audio_board_handle_t    board_handle;
    audio_event_iface_handle_t evt;

} RTPCONFIG, * PRTPCONFIG;

typedef enum
{
	rtpCMD_setState= 0,
    rtpCMD_setVolume,
    rtpCMD_setMulticastAddress
} rtpCMD;

#define ENABLE 1
#define DISABLE 0

void _setVolume_num(audio_board_handle_t board_handle, uint8_t vol) {
	if(vol>100){
		vol=100;
	}
	audio_hal_set_volume(board_handle->audio_hal, vol);
}

void pipelineStop(PRTPCONFIG c){
    if (c->pipeline != NULL) {
        audio_pipeline_stop(c->pipeline);
        
        // ESP_LOGD(TAG, "[audioLAN_] stop pipline");
        audio_pipeline_wait_for_stop(c->pipeline);
        // ESP_LOGD(TAG, "[audioLAN_] stop pipline OK");
        audio_pipeline_terminate(c->pipeline);
        //}

        // Принудительно удаляем задачи элементов
        // audio_element_deinit(c->rtp_stream_reader);
        // audio_element_deinit(c->i2s_stream_writer);

        // ESP_LOGD(TAG, "[audioLAN_] stop terminate");
        if (c->rtp_stream_reader != NULL) {
            audio_pipeline_unregister(c->pipeline, c->rtp_stream_reader);
        }
        // ESP_LOGD(TAG, "[audioLAN_] stop audio_pipeline_unregister RTP");
        if (c->i2s_stream_writer != NULL) {
            audio_pipeline_unregister(c->pipeline, c->i2s_stream_writer);
        }

        
        // ESP_LOGD(TAG, "[audioLAN_] stop audio_pipeline_unregister I2S");
        audio_pipeline_deinit(c->pipeline);
        c->pipeline = NULL;
        c->rtp_stream_reader = NULL;
        c->i2s_stream_writer = NULL;
        // ESP_LOGD(TAG, "[audioLAN_] stop audio_pipeline_DEINIT");
    }

    // Free the host string allocated in pipelineStart
    if (c->host) {
        free(c->host);
        c->host = NULL;
    }

    // Note: Individual elements are deinitialized by audio_pipeline_deinit
    // Do not deinitialize them separately to avoid double-free errors

    // Destroy event interface to prevent memory leaks
    if (c->evt != NULL) {
        audio_event_iface_destroy(c->evt);
        c->evt = NULL;
    }

    // Deinitialize board handle to prevent memory leaks
    if (c->board_handle != NULL) {
        // Stop the codec before deinitializing the board
        audio_hal_ctrl_codec(c->board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_STOP);
        audio_board_deinit(c->board_handle);
        c->board_handle = NULL;
    }
}

esp_err_t pipelineStart(PRTPCONFIG c) {
	//uint32_t heapBefore = xPortGetFreeHeapSize();

	audio_element_state_t el_state = audio_element_get_state(c->i2s_stream_writer);
	if(el_state==AEL_STATE_RUNNING){
		pipelineStop(c);
	}

    ESP_LOGI(TAG, "[ 1 ] Start codec chip");
    // Check if board handle already exists to prevent memory leak
    if (c->board_handle != NULL) {
        // Stop the codec before deinitializing
        audio_hal_ctrl_codec(c->board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_STOP);
        audio_board_deinit(c->board_handle);
        c->board_handle = NULL;
    }
    c->board_handle = audio_board_init();
    audio_hal_ctrl_codec(c->board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    // Check if pipeline already exists to prevent memory leak
    if (c->pipeline != NULL) {
        // First, stop the existing pipeline if it's running
        pipelineStop(c);
    }
    // At this point, all previous resources should be cleaned up by pipelineStop
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    c->pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(c->pipeline);
    //AUDIO_NULL_CHECK(TAG, c->pipeline, return ESP_FAIL);

    ESP_LOGI(TAG, "[2.1] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.use_alc = false;
    i2s_cfg.task_core=1;
    //i2s_cfg.buffer_len = 36000;
    c->i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.2] Create rtp client stream to read data");
    // Free existing host string if allocated to prevent memory leak
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
    rtp_stream_cfg_t rtp_cfg = RTP_STREAM_CFG_DEFAULT();
    rtp_cfg.type = AUDIO_STREAM_READER;
    rtp_cfg.port = c->port;
    rtp_cfg.host = c->host;  /* NULL for unicast, multicast address for multicast */
    rtp_cfg.task_core = 0;
    rtp_cfg.buf_size = 0; // use default audio_element buffer
    rtp_cfg.jbuf_ms = c->jbuf_ms;
    rtp_cfg.sample_rate = c->sample_rate;
    rtp_cfg.bits_per_sample = c->bits_per_sample;
    c->rtp_stream_reader = rtp_stream_init(&rtp_cfg);
    AUDIO_NULL_CHECK(TAG, c->rtp_stream_reader, return ESP_FAIL);

    ESP_LOGI(TAG, "[2.3] Register all elements to audio pipeline");
    audio_pipeline_register(c->pipeline, c->rtp_stream_reader, "rtp");

    audio_pipeline_register(c->pipeline, c->i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.4] Link it together rtp-->i2s");
    audio_pipeline_link(c->pipeline, (const char *[]) {"rtp", "i2s"}, 2);

    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    // Check if event interface already exists to prevent memory leak
    if (c->evt != NULL) {
        audio_event_iface_destroy(c->evt);
        c->evt = NULL;
    }
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    c->evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(c->pipeline, c->evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(c->pipeline);

    i2s_stream_set_clk(c->i2s_stream_writer, c->sample_rate, c->bits_per_sample, 2);

	return ESP_OK;
}

/* Модуль звук через сеть. 
- Поддерживает как unicast, так и multicast режимы (настраивается через конфиг)
- Читает аудио данные из RTP потока и выводит на I2S
- Поддерживает горячее переключение multicast адреса на лету через команду setMulticastAddress
- задержка порядка 70мс
*/
void configure_audioLAN(PRTPCONFIG c, int slot_num)
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
    ESP_LOGD(TAG, "[LANplayer_%d] volume:%d", slot_num, c->volume);

    /* Multicast адрес
    - если указан, модуль работает в режиме multicast
    - если не указан, модуль слушает unicast на порту
    - формат: IPv4 адрес, например \"239.0.7.1\"
	*/
    if (strstr(me_config.slot_options[slot_num], "multicastAddress") != NULL) {
        char * mcast_addr = get_option_string_val(slot_num, "multicastAddress", "239.0.7.0");
        c->multicastAddress = strdup(mcast_addr);
        c->useMulticast = true;
        ESP_LOGI(TAG, "[LANplayer_%d] multicast mode: %s", slot_num, c->multicastAddress);
    } else {
        c->multicastAddress = NULL;
        c->useMulticast = false;
        ESP_LOGI(TAG, "[LANplayer_%d] unicast mode", slot_num);
    }

    /* порт
    - по умолчанию 7777
	*/
	c->port =  get_option_int_val(slot_num, "port", "num", 7777, 0, UINT16_MAX);
    ESP_LOGD(TAG, "[LANplayer_%d] port:%d", slot_num, c->port);

    /* Sample rate
    - по умолчанию 48000
	*/
	c->sample_rate = get_option_int_val(slot_num, "sampleRate", "num", 48000, 8000, 96000);
    ESP_LOGD(TAG, "[LANplayer_%d] sampleRate:%d", slot_num, c->sample_rate);

    /* Bits per sample
    - по умолчанию 16
	*/
	c->bits_per_sample = get_option_int_val(slot_num, "bitsPerSample", "num", 16, 8, 32);
    ESP_LOGD(TAG, "[LANplayer_%d] bitsPerSample:%d", slot_num, c->bits_per_sample);

    /* Размер jitter-буфера в миллисекундах
    - по умолчанию 20 мс
	*/
	c->jbuf_ms = get_option_int_val(slot_num, "bufSize", "num", 20, 20, 200);
    ESP_LOGD(TAG, "[LANplayer_%d] bufSize:%d ms", slot_num, c->jbuf_ms);

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

    /* Рапортует состояние модуля вкл/выкл
	*/
	c->stateReport = stdreport_register(RPTT_int, slot_num, "state", "state");

    /* Рапортует при изменении громкости
	*/
	c->volumeReport = stdreport_register(RPTT_int, slot_num, "percent", "volume");

    /* Рапортует текущий адрес стрима (multicast/unicast)
	*/
	c->addressReport = stdreport_register(RPTT_string, slot_num, "string", "address");

    /* Команда включает/выключает плеер
    */
    stdcommand_register(&c->cmds, rtpCMD_setState, "setState", PARAMT_int);

    /* Команда устанавливает значение громкости
    0-100
    */
    stdcommand_register(&c->cmds, rtpCMD_setVolume, "setVolume", PARAMT_int);

    /* Команда переключает multicast адрес на лету
    - строка IPv4 адреса для multicast, например \"239.0.7.1\"
    - \"0\" — переключиться на unicast
    */
    stdcommand_register(&c->cmds, rtpCMD_setMulticastAddress, "setMulticastAddress", PARAMT_string);
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
    pipelineStart(c);
    _setVolume_num(c->board_handle, c->volume);
    if(c->state == ENABLE){
        audio_pipeline_resume(c->pipeline);
        ESP_LOGD(TAG,"audioLAN_%d state ENABLE", slot_num);
    }else{
        audio_pipeline_pause(c->pipeline);
        ESP_LOGD(TAG,"audioLAN_%d slot state DISABLE", slot_num);
    }

    stdreport_i(c->stateReport, c->state);
    stdreport_i(c->volumeReport, c->volume);
    { char _ab[48]; snprintf(_ab, sizeof(_ab), "%s:%d", c->useMulticast ? c->multicastAddress : _get_device_ip(), c->port); stdreport_s(c->addressReport, _ab); }

    //TickType_t lastWakeTime = xTaskGetTickCount();

    int64_t last_blink_time = 0;
    int led_state = 0;

#if CONFIG_LWIP_STATS
    int64_t last_stats_time = 0;
    uint32_t prev_udp_drop = 0;
    uint32_t prev_udp_recv = 0;
#endif

    while (1) {
        //ESP_LOGD(TAG, "[audioLAN_%d] I am alive", slot_num);

        //LED Logic
        if (c->state == DISABLE) {
            gpio_set_level(led_pin, 0);
            //ESP_LOGD(TAG, "Slot_%d set MODE light OFF", slot_num);
        } else {
            if (rtp_stream_check_connection(c->rtp_stream_reader, 500) == ESP_OK) {
                gpio_set_level(led_pin, 1);
                //ESP_LOGD(TAG, "Slot_%d set MODE light ON", slot_num);
            } else {
                //ESP_LOGD(TAG, "Slot_%d set MODE light BLINK", slot_num);
                if ((esp_timer_get_time() - last_blink_time) > 500000) {
                    led_state = !led_state;
                    gpio_set_level(led_pin, led_state);
                    last_blink_time = esp_timer_get_time();
                }
            }
        }

#if CONFIG_LWIP_STATS
        // Log UDP drop stats every 10 seconds
        if ((esp_timer_get_time() - last_stats_time) > 10000000) {
            uint32_t cur_drop = lwip_stats.udp.drop;
            uint32_t cur_recv = lwip_stats.udp.recv;
            if (cur_drop != prev_udp_drop) {
                ESP_LOGW(TAG, "[UDP stats] recv:%u drop:%u (+%u) link_drop:%u",
                         (unsigned)cur_recv, (unsigned)cur_drop, (unsigned)(cur_drop - prev_udp_drop),
                         (unsigned)lwip_stats.link.drop);
            }
            prev_udp_drop = cur_drop;
            prev_udp_recv = cur_recv;
            last_stats_time = esp_timer_get_time();
        }
#endif
        
        int cmd = stdcommand_receive(&c->cmds, &params, 150);
		char * cmd_arg = (params.count > 0) ? params.p[0].p : (char *)"0";

        switch (cmd){
            case -1: // none
                break;

            case rtpCMD_setState:
                if((atoi(cmd_arg)==ENABLE)&&(c->state != ENABLE)){
                    audio_pipeline_resume(c->pipeline);
                }else if((atoi(cmd_arg)==DISABLE)&&(c->state != DISABLE)){
                    audio_pipeline_pause(c->pipeline);
                }
                c->state = atoi(cmd_arg);
                ESP_LOGD(TAG, "[audioLAN_%d] lets set state:%d. Free heap:%d", slot_num, c->state, xPortGetFreeHeapSize());
                stdreport_i(c->stateReport, c->state);
                break;

            case rtpCMD_setVolume:
                c->volume = atoi(cmd_arg);
                _setVolume_num(c->board_handle, c->volume);
                ESP_LOGD(TAG, "[audioLAN_%d] setVolume:%d. Free heap:%d", slot_num, c->volume, xPortGetFreeHeapSize());
                stdreport_i(c->volumeReport, c->volume);
                break;

            case rtpCMD_setMulticastAddress:
                {
                    bool switchToUnicast = (strcmp(cmd_arg, "0") == 0);

                    if (switchToUnicast) {
                        /* Переключение на unicast — перезапуск пайплайна без multicast адреса */
                        ESP_LOGI(TAG, "[audioLAN_%d] Switching to unicast mode", slot_num);
                        if (c->multicastAddress) {
                            free(c->multicastAddress);
                            c->multicastAddress = NULL;
                        }
                        c->useMulticast = false;
                        pipelineStop(c);
                        pipelineStart(c);
                        if (c->state == ENABLE) {
                            audio_pipeline_resume(c->pipeline);
                        } else {
                            audio_pipeline_pause(c->pipeline);
                        }
                        { char _ab[48]; snprintf(_ab, sizeof(_ab), "%s:%d", _get_device_ip(), c->port); stdreport_s(c->addressReport, _ab); }
                    } else {
                        /* Переключение на новый multicast адрес */
                        ESP_LOGI(TAG, "[audioLAN_%d] Switching to multicast: %s", slot_num, cmd_arg);

                        if (c->useMulticast && c->rtp_stream_reader) {
                            /* Уже в multicast режиме — попробовать горячее переключение */
                            char *new_addr = strdup(cmd_arg);
                            esp_err_t switch_result = rtp_stream_switch_multicast_address(c->rtp_stream_reader, new_addr, c->port);

                            if (switch_result == ESP_OK) {
                                ESP_LOGI(TAG, "[audioLAN_%d] Hot-switched to %s", slot_num, new_addr);
                                if (c->multicastAddress) free(c->multicastAddress);
                                c->multicastAddress = new_addr;
                                if (c->host) free(c->host);
                                c->host = strdup(new_addr);
                                { char _ab[32]; snprintf(_ab, sizeof(_ab), "%s:%d", new_addr, c->port); stdreport_s(c->addressReport, _ab); }
                            } else {
                                ESP_LOGW(TAG, "[audioLAN_%d] Hot-switch failed, restarting pipeline", slot_num);
                                free(new_addr);
                                if (c->multicastAddress) free(c->multicastAddress);
                                c->multicastAddress = strdup(cmd_arg);
                                c->useMulticast = true;
                                pipelineStop(c);
                                pipelineStart(c);
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
                            pipelineStop(c);
                            pipelineStart(c);
                            if (c->state == ENABLE) {
                                audio_pipeline_resume(c->pipeline);
                            } else {
                                audio_pipeline_pause(c->pipeline);
                            }
                            { char _ab[32]; snprintf(_ab, sizeof(_ab), "%s:%d", c->multicastAddress, c->port); stdreport_s(c->addressReport, _ab); }
                        }
                    }
                    ESP_LOGD(TAG, "[audioLAN_%d] setMulticastAddress done. multicast:%d host:%s Free heap:%d",
                             slot_num, c->useMulticast, c->host ? c->host : "(unicast)", xPortGetFreeHeapSize());
                }
                break;
        }

        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(c->evt, &msg, 120);
        if (ret != ESP_OK) {
            //ESP_LOGE(TAG, "[ * ] Event interface error : %d cmd:%d", ret, msg.cmd);
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
	xTaskCreate(audioLAN_task, tmpString, 1024 * 16, &t_slot_num, configMAX_PRIORITIES - 20, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "audioLAN_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


const char * get_manifest_audioLAN()
{
	return manifesto;
}