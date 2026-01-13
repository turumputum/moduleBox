// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdio.h>
#include "audioPlayer.h"

#include "esp_log.h"

#include "esp_task_wdt.h"
#include "esp_vfs_fat.h"
//#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_mem.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "filter_resample.h"
#include "equalizer.h"
#include "string.h"
#include <stdreport.h>
#include <stdcommand.h>

#include "audio_event_iface.h"
#include "periph_wifi.h"

#include "reporter.h"
#include "executor.h"

#include "stateConfig.h"
#include "me_slot_config.h"

#include "board.h"
#include "audio_sonic.h"
#include "esp_audio.h"

#include <generated_files/gen_audioPlayer.h>

extern uint8_t ab_ver;

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#undef  LOG_LOCAL_LEVEL 
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct __tag_AUDIOCONFIG
{
	int8_t 					volume;
	float 					speed;
	float 					tone;
	uint8_t 				eqFlag;
	int8_t 					eqLow;
	int8_t 					eqMid;
	int8_t 					eqHigh;
	int 					attenuation;
	uint16_t 				play_delay;
	uint8_t 				play_to_end;

    STDCOMMANDS             cmds;

	int						ETreport;
} AUDIOCONFIG, * PAUDIOCONFIG; 

typedef enum
{
	MYCMD_play = 0,
	MYCMD_stop,
	MYCMD_shift,
	MYCMD_setVolume
} MYCMD;

static const char *TAG = "AUDIO";

extern uint8_t SLOTS_PIN_MAP[10][4];

audio_pipeline_handle_t pipeline;
audio_board_handle_t board_handle;
audio_element_handle_t i2s_stream_writer, mp3_decoder, fatfs_stream_reader, rsp_handle, equalizer;
char *url = NULL;
//playlist_operator_handle_t sdcard_list_handle = NULL;

audio_event_iface_handle_t evt;

extern stateStruct me_state;
extern configuration me_config;

void audioSetIndicator(uint8_t slot_num, uint32_t level){
	gpio_num_t led_pin = SLOTS_PIN_MAP[slot_num][3];
	gpio_set_level(led_pin, level);// module led light
}

void trackShift(char* cmd_arg){
	//ESP_LOGD(TAG,"trackShift arg:%d", cmd_arg[0]);
	if(strlen(cmd_arg)==0){return;}
	if (cmd_arg[0] == 43) {
		me_state.currentTrack += atoi(cmd_arg + 1);
		if (me_state.currentTrack >= me_state.numOfTrack) {
			me_state.currentTrack = 0;
		}
		ESP_LOGD(TAG, "Shift track+. Current track index: %d", me_state.currentTrack);
	} else if (cmd_arg[0] == 45) {
		int newTrack = (int)me_state.currentTrack - atoi(cmd_arg + 1);		
		me_state.currentTrack = newTrack < 0 ? me_state.numOfTrack-1 : newTrack;
		ESP_LOGD(TAG, "Shift track-. Current track index: %d", me_state.currentTrack);
	} else if (cmd_arg[0] == 35) {
		ESP_LOGD(TAG, "noShift track. Current track index: %d", me_state.currentTrack);
	}else if (strstr(cmd_arg, "random")!=NULL) {
		me_state.currentTrack = rand() % me_state.numOfTrack;
		ESP_LOGD(TAG, "Shift track. Set random track index: %d", me_state.currentTrack);
	}else {
		me_state.currentTrack = atoi(cmd_arg);
		if (me_state.currentTrack >= me_state.numOfTrack) {
			me_state.currentTrack = 0;
		}
		ESP_LOGD(TAG, "Shift track. Current track index: %d of: %d ", me_state.currentTrack, me_state.numOfTrack);
	}
}

void fill_equalizer_gains(int low_gain, int mid_gain, int high_gain, int *set_gain) {
    // Fill 20 bands with smooth transitions
    set_gain[0] = low_gain;    // Low start
    set_gain[10] = low_gain;
	set_gain[1] = low_gain + (mid_gain - low_gain)/3;
	set_gain[11] = low_gain + (mid_gain - low_gain)/3;
    set_gain[2] = low_gain + ((mid_gain - low_gain)*2)/3;
	set_gain[12] = low_gain + ((mid_gain - low_gain)*2)/3;
    set_gain[3] = mid_gain;    // Mid start
	set_gain[13] = mid_gain;    // Mid start
    set_gain[4] = mid_gain;
	set_gain[14] = mid_gain;
    set_gain[5] = mid_gain;
	set_gain[15] = mid_gain;
    set_gain[6] = mid_gain + (high_gain - mid_gain)/3;
	set_gain[16] = mid_gain + (high_gain - mid_gain)/3;
    set_gain[7] = mid_gain + ((high_gain - mid_gain)*2)/3;
	set_gain[17] = mid_gain + ((high_gain - mid_gain)*2)/3;
    set_gain[8] = high_gain;   // High start
	set_gain[18] = high_gain;   // High start
    set_gain[9] = high_gain;
	set_gain[19] = high_gain;
}
/*
    Звуковой Модуль
    slots: 0
*/
void configure_audioPlayer(PAUDIOCONFIG c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* Уровень громкости
    */
	c->volume = get_option_int_val(slot_num, "volume", "", 70, 1, 4096);
	if(c->volume>100){c->volume=100;}
	if(c->volume<0){c->volume=0;}
	ESP_LOGD(TAG, "Set volume:%d", c->volume);

	/* Скорость воспроизведения
	*/
	c->speed = get_option_float_val(slot_num, "speed", 1.09);
	ESP_LOGD(TAG, "Set speed:%f", c->speed);

	/* Сдвиг тональности
	*/
	c->tone = get_option_float_val(slot_num, "tone", 1.0);
	ESP_LOGD(TAG, "Set tone:%f", c->tone);

	/* Нижняя граница эквалайзера
	*/
	if ((c->eqLow = get_option_int_val(slot_num, "eqLow", "", -13, -20, 0)) != 0)
	{
		c->eqFlag = 1;
		ESP_LOGD(TAG, "Set eqLow:%d", c->eqLow);
	}

	/* Средняя граница эквалайзера
	*/
	if ((c->eqMid = get_option_int_val(slot_num, "eqMid", "", -13, -20, 0)) != 0)
	{
		c->eqFlag = 1;
		ESP_LOGD(TAG, "Set eqMid:%d", c->eqMid);
	}

	/* Верхняя граница эквалайзера
	*/
	if ((c->eqHigh = get_option_int_val(slot_num, "eqHigh", "", -13, -20, 0)) != 0)
	{
		c->eqFlag = 1;
		ESP_LOGD(TAG, "Set eqMid:%d", c->eqMid);
	}

	/* Использовать затухание
	*/
	c->attenuation = get_option_flag_val(slot_num, "attenuation");
	if (c->attenuation != 0) {
		ESP_LOGD(TAG, "Enable attenuation");
	}
	
	/* Пауза перед проигрыванием
	*/
	c->play_delay = get_option_int_val(slot_num, "playDelay", "", 0, 0, 4096);
	ESP_LOGD(TAG, "Set play_delay:%d", c->play_delay);


	/* Проигрывать до конца
	*/
	c->play_to_end = get_option_flag_val(slot_num, "playToEnd");
	ESP_LOGD(TAG, "Set play_to_end:%d", c->play_to_end);

	//---add action to topic list---
	if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic", "/player_0");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/player_0")+3];
		sprintf(t_str, "%s/player_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	}

	/* Рапортует номер трека при завершении
	*/
	c->ETreport = stdreport_register(RPTT_string, slot_num, "", "endOfTrack");


    /* Проиграть трек
       Опционально - номер трека
    */
    stdcommand_register(&c->cmds, MYCMD_play, "play", PARAMT_string);

    /* Остановить проигрывание
       
    */
    stdcommand_register(&c->cmds, MYCMD_stop, "stop", PARAMT_none);

    /* Переключить трек
       
    */
   	stdcommand_register(&c->cmds, MYCMD_shift, "shift", PARAMT_string);

    /* Установить громкость
       
    */
	stdcommand_register(&c->cmds, MYCMD_setVolume, "setVolume", PARAMT_int);


}

void audio_task(void *arg) {
    PAUDIOCONFIG c = calloc(1, sizeof(AUDIOCONFIG));

	int slot_num = *(int*) arg;
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();
    STDCOMMAND_PARAMS       params = { 0 };

	params.skipTypeChecking = true;

	gpio_num_t led_pin = SLOTS_PIN_MAP[slot_num][3];
	//ESP_LOGD(TAG, "Start codec chip");

	me_state.command_queue[slot_num] = xQueueCreate(25, sizeof(command_message_t));

	//int16_t currentTrack=0;

	configure_audioPlayer(c, slot_num);


	esp_rom_gpio_pad_select_gpio(led_pin);
	gpio_set_direction(led_pin, GPIO_MODE_OUTPUT);
	gpio_set_level(led_pin, 0);

	board_handle = audio_board_init();// TO_DO raspetrushit'
	audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

	//ESP_LOGD(TAG, "Create audio pipeline for playback");
	audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
	//pipeline_cfg.rb_size = 8 * 1024;//3
	pipeline = audio_pipeline_init(&pipeline_cfg);
	mem_assert(pipeline);

	if(me_config.boardVersion==4){
		ab_ver = 4;
	}
	//ESP_LOGD(TAG, "Create i2s stream to write data to codec chip");
	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
	i2s_cfg.out_rb_size = 12 * 1024;
	i2s_cfg.task_core = 1;
	// i2s_cfg.i2s_config.sample_rate = 48000;
	// i2s_cfg.i2s_config.dma_buf_count = 3; //3
	// i2s_cfg.i2s_config.dma_buf_len = 300; //300
	i2s_cfg.type = AUDIO_STREAM_WRITER;
	//i2s_cfg.task_prio = configMAX_PRIORITIES-1; //23
	i2s_cfg.use_alc = true;
	i2s_cfg.volume = -34 + (c->volume / 3);
	i2s_cfg.stack_in_ext = true;
	i2s_stream_writer = i2s_stream_init(&i2s_cfg);

	//ESP_LOGD(TAG, "Create fatfs stream to read data from sdcard");
	fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
	fatfs_cfg.type = AUDIO_STREAM_READER;
	fatfs_cfg.ext_stack = false;
	fatfs_cfg.task_stack = 4096; // Increase stack size
	//fatfs_cfg.task_prio = 22; //22
	fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);

	//ESP_LOGD(TAG, "Create mp3 decoder to decode mp3 file");
	mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
	mp3_cfg.stack_in_ext = false;
	//mp3_cfg.task_prio = 22; //22
	//mp3_cfg.out_rb_size = 3 * 1024;
	mp3_decoder = mp3_decoder_init(&mp3_cfg);

	//ESP_LOGD(TAG, "Create resample filter");
	rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
	rsp_cfg.stack_in_ext = false;
	//rsp_cfg.task_prio = 22; //22
	rsp_cfg.prefer_flag = 1;
	rsp_handle = rsp_filter_init(&rsp_cfg);

	sonic_cfg_t sonic_cfg = DEFAULT_SONIC_CONFIG();
    sonic_cfg.sonic_info.samplerate = 48000;
    sonic_cfg.sonic_info.channel = 2;
    sonic_cfg.sonic_info.resample_linear_interpolate = 1;
    audio_element_handle_t sonic_el = sonic_init(&sonic_cfg);
	sonic_set_pitch_and_speed_info(sonic_el, c->tone, c->speed);

	equalizer_cfg_t eq_cfg = DEFAULT_EQUALIZER_CONFIG();
    int set_gain[] = { -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13, -13};
    fill_equalizer_gains(c->eqLow, c->eqMid, c->eqHigh, set_gain);
	eq_cfg.channel = 2;
	eq_cfg.set_gain = set_gain; // The size of gain array should be the multiplication of NUMBER_BAND and number channels of audio stream data. The minimum of gain is -13 dB.
    equalizer = equalizer_init(&eq_cfg);

	//ESP_LOGD(TAG, "Register all elements to audio pipeline");
	audio_pipeline_register(pipeline, fatfs_stream_reader, "file");
	audio_pipeline_register(pipeline, mp3_decoder, "mp3");
	//audio_pipeline_register(pipeline, rsp_handle, "filter");
	audio_pipeline_register(pipeline, sonic_el, "sonic");//-----
	audio_pipeline_register(pipeline, equalizer, "equalizer");//-----
	audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

	//ESP_LOGD(TAG, "Link it together [sdcard]-->fatfs_stream-->mp3_decoder-->resample-->i2s_stream-->[codec_chip]");
	//const char *link_tag[4] = { "file", "mp3", "filter", "i2s" };
	//const char *link_tag[5] = { "file", "mp3", "filter", "sonic", "i2s" };
	const char *link_tag[6] = { "file", "mp3", "sonic", "equalizer", "i2s"};
	//const char *link_tag[6] = { "file", "mp3", "filter", "sonic", "equalizer", "i2s"};
	//audio_pipeline_link(pipeline, &link_tag[0], 4);
	audio_pipeline_link(pipeline, &link_tag[0], 5);
	//audio_pipeline_link(pipeline, &link_tag[0], 6);

	

	//ESP_LOGD(TAG, "Set up  event listener");
	audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
	evt_cfg.external_queue_size = 40;//5
	evt_cfg.internal_queue_size = 40;//5
	evt_cfg.queue_set_size = 40;//5
	evt = audio_event_iface_init(&evt_cfg);

	//xTaskCreatePinnedToCore(listenListener, "audio_listener", 1024 * 4, NULL, 1, NULL, 0);
	audio_pipeline_set_listener(pipeline, evt);
	ESP_LOGD(TAG, "Audio init complite. Duration: %ld ms. Heap usage: %lu free Heap:%u", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize(),
			xPortGetFreeHeapSize());

	
	vTaskDelay(100);

	int att_flag=0;
	uint8_t att_vol=c->volume;

	char reportStr[strlen("/endOfTrack")+10];
	//listen audio event
	audio_event_iface_msg_t msg;
	esp_err_t ret;
	audio_element_state_t el_state;

	waitForWorkPermit(slot_num);


	while(1)
	{
		// if (xQueueReceiveLast(me_state.command_queue[slot_num], &cmd, 5) == pdPASS)
		int cmd = stdcommand_receive(&c->cmds, &params, 5);
		char * cmd_arg = (params.count > 0) ? params.p[0].p : (char *)"0";

        switch (cmd)
        {
            case -1: // none
                break;

            case MYCMD_play:
				//ESP_LOGD(TAG, "AEL status:%d currentTrack:%d", audio_element_get_state(i2s_stream_writer), c->currentTrack);
				if((audio_element_get_state(i2s_stream_writer)==AEL_STATE_RUNNING)&&(c->play_to_end==1)){
					ESP_LOGD(TAG, "skip restart track");
				}else{
					ESP_LOGD(TAG, "before shift:%d, cmd_arg = '%s'", me_state.currentTrack, cmd_arg);
					trackShift(cmd_arg);
					ESP_LOGD(TAG, "after shift:%d", me_state.currentTrack);
					vTaskDelay(pdMS_TO_TICKS(c->play_delay));
					if(audioPlay(me_state.currentTrack)==ESP_OK){
						audioSetIndicator(slot_num, 1);
					}else{
						audioSetIndicator(slot_num, 0);
					}
					setVolume_num(c->volume);
				}
				break;
 
            case MYCMD_stop:
				if(c->attenuation!=0){
					att_flag=1;
					//ESP_LOGD(TAG, "attenuation start");
				}else{
					audioStop();
					audioSetIndicator(slot_num, 0);
				}
				break;

            case MYCMD_shift:
				trackShift(cmd_arg);
				if((audio_element_get_state(i2s_stream_writer)==AEL_STATE_RUNNING)){
					if(c->play_to_end==0){
						if(audioPlay(me_state.currentTrack)==ESP_OK){
							audioSetIndicator(slot_num, 1);
						}else{
							audioSetIndicator(slot_num, 0);
						}
					}
				}
				break;

            case MYCMD_setVolume:
				if ((params.count > 0) && (params.p[0].type == PARAMT_int))
				{
					setVolume_num(params.p[0].i);
				}
				break;
		}


		if(att_flag==1){
			att_vol-=1;
			//ESP_LOGD(TAG, "attenuation vol:%d", att_vol);
			setVolume_num(att_vol);
			if(att_vol==0){
				audioStop();
				audioSetIndicator(slot_num, 0);
				att_vol = c->volume;
				att_flag=0;
			}
		}

		

		//listen audio event mp3_decoder
		el_state = audio_element_get_state(mp3_decoder);
		if(el_state == AEL_STATE_ERROR){
			ESP_LOGE(TAG, "mp3_decoder Error state: %d", el_state);
			esp_restart();
		}

		//listen audio event i2s_stream_writer
		ret = audio_event_iface_listen(evt, &msg, 0);
		if(ret == ESP_OK)
		{
		 	//ESP_LOGD(TAG, "audio_Event: %d el_state: %d", msg.cmd, el_state);
			if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
				el_state = audio_element_get_state(i2s_stream_writer);
				if (el_state == AEL_STATE_FINISHED) {
					audioStop();
					audioSetIndicator(slot_num, 0);
					//ESP_LOGD(TAG, "endOfTrack EVENT !!!!!!!!!!!!!!!!!");
					vTaskDelay(pdMS_TO_TICKS(10));
					memset(reportStr, 0, strlen(reportStr));
					sprintf(reportStr,"%d", me_state.currentTrack);
					//report(reportStr, slot_num);
					stdreport_s(c->ETreport, reportStr);
				}
			}
			if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) 
			{
				if (msg.source == (void *) mp3_decoder && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) 
				{
					audio_element_info_t music_info = {0};
					audio_element_getinfo(mp3_decoder, &music_info);

					ESP_LOGD(TAG, "Current track: sample_rates=%d, bits=%d, ch=%d", 
							music_info.sample_rates, 
							music_info.bits, 
							music_info.channels);

					// ESP_LOGI(TAG, "[ * ] Received music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
					// 		music_info.sample_rates, music_info.bits, music_info.channels);
					audio_element_setinfo(i2s_stream_writer, &music_info);
					rsp_filter_set_src_info(rsp_handle, music_info.sample_rates, music_info.channels);
				}
			}
		}
	}
}

void audioInit(uint8_t slot_num){
	//uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_player_%d", slot_num);
	xTaskCreatePinnedToCore(audio_task, tmpString, 1024*8, &t_slot_num,configMAX_PRIORITIES-5, NULL, 0);
}

void audioDeinit(void) {
	audio_pipeline_unregister(pipeline, mp3_decoder);
	audio_pipeline_unregister(pipeline, i2s_stream_writer);
	audio_pipeline_unregister(pipeline, rsp_handle);

	audio_pipeline_deinit(pipeline);
	//audio_element_deinit(fatfs_stream_reader);
	audio_element_deinit(i2s_stream_writer);
	audio_element_deinit(mp3_decoder);
	audio_element_deinit(rsp_handle);
}

void setVolume_num(uint8_t vol) {
	if(vol>100){
		vol=100;
	}
	i2s_alc_volume_set(i2s_stream_writer, -34 + (vol / 3));
}

void setVolume_str(char *cmd){

}

esp_err_t audioPlay(uint8_t truckNum) {
	//uint32_t heapBefore = xPortGetFreeHeapSize();
	
	audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
	if(el_state==AEL_STATE_RUNNING){
		audioStop();
	}

	ESP_ERROR_CHECK(audio_element_set_uri(fatfs_stream_reader, me_config.soundTracks[truckNum]));
	ESP_ERROR_CHECK(audio_pipeline_reset_ringbuffer(pipeline));
	ESP_ERROR_CHECK(audio_pipeline_reset_elements(pipeline));
	ESP_ERROR_CHECK(audio_pipeline_change_state(pipeline, AEL_STATE_INIT));
	ESP_ERROR_CHECK(audio_pipeline_run(pipeline));

	ESP_LOGD(TAG, "Playing file: %s", me_config.soundTracks[truckNum]);


	return ESP_OK;
}

void audioStop(void) {
	//audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
	//ESP_LOGD(TAG, "audioStop state: %d", el_state);
	//if ((el_state != AEL_STATE_FINISHED) && (el_state != AEL_STATE_STOPPED)) {
		audio_pipeline_stop(pipeline);
		audio_pipeline_wait_for_stop(pipeline);
	//}
	ESP_ERROR_CHECK(audio_pipeline_terminate(pipeline));


	ESP_LOGD(TAG, "Stop playing. Free heap:%d", xPortGetFreeHeapSize());
}

void audioPause(void) {

	ESP_LOGD(TAG, "Pausing audio pipeline");
	audio_pipeline_pause(pipeline);
}

const char * get_manifest_audioPlayer()
{
	return manifesto;
}

// void listenListener(void *pvParameters) {
// 	while (1) {
// 		audio_event_iface_msg_t msg;
// 		esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
// 		if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
// 			audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
// 			if (el_state == AEL_STATE_FINISHED) {
// 				audioStop();
				
// 				vTaskDelay(pdMS_TO_TICKS(10));
// 				char tmpStr[strlen(me_config.deviceName)+strlen("/play_end")+10];
// 				sprintf(tmpStr,"%s/play_end:%d", me_config.deviceName, me_state.currentTrack);
// 				report(tmpStr, 0);
				
// 			}
// 		}

// 	}

// }
