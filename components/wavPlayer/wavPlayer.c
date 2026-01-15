// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdio.h>
#include "wavPlayer.h"

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

#include "wav_handle.h"

#include <generated_files/gen_wavPlayer.h>


// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#undef  LOG_LOCAL_LEVEL 
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct __tag_WAVPLAYERCONFIG
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

	wav_handle_t 			handler;
} WAVPLAYERCONFIG, * PWAVPLAYERCONFIG; 

typedef enum
{
	MYCMD_play = 0,
	MYCMD_stop,
	MYCMD_shift,
	MYCMD_setVolume
} MYCMD;

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

static const char *TAG = "WAV";

extern uint8_t SLOTS_PIN_MAP[10][4];


extern stateStruct me_state;
extern configuration me_config;

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

static void trackShift(char* cmd_arg);
static esp_err_t audioPlay(wav_handle_t h, uint8_t truckNum);
static void audioSetIndicator(uint8_t slot_num, uint32_t level);
static void setVolume_num(uint8_t vol);
static void audioStop(wav_handle_t h);

/*
    Звуковой модуль для несжатых WAV-файлов
    slots: 0
*/
void configure_wavPlayer(PWAVPLAYERCONFIG c, int slot_num)
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

void wavplayer_task(void *arg) {
    PWAVPLAYERCONFIG c = calloc(1, sizeof(WAVPLAYERCONFIG));
	//esp_wav_player_config_t player_conf = ESP_WAV_PLAYER_DEFAULT_CONFIG();
	
	int slot_num = *(int*) arg;
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();
    STDCOMMAND_PARAMS       params = { 0 };

	params.skipTypeChecking = true;

	gpio_num_t led_pin = SLOTS_PIN_MAP[slot_num][3];
	//ESP_LOGD(TAG, "Start codec chip");

	me_state.command_queue[slot_num] = xQueueCreate(25, sizeof(command_message_t));

	int16_t currentTrack=0;

	configure_wavPlayer(c, slot_num);

	
	c->handler = wav_handle_init(TAG);
	
   	// esp_wav_player_init(&c->wav_player, &player_conf);
    // esp_wav_player_set_volume(c->wav_player, 50);

	ESP_LOGD(TAG, "Audio init complite. Duration: %ld ms. Heap usage: %lu free Heap:%u", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize(),
			xPortGetFreeHeapSize());

	
//	vTaskDelay(100);

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
		int cmd = stdcommand_receive(&c->cmds, &params, pdMS_TO_TICKS(1));
		char * cmd_arg = (params.count > 0) ? params.p[0].p : (char *)"0";

        switch (cmd)
        {
            case -1: // none
                break;

            case MYCMD_play:
				ESP_LOGD(TAG, "AEL status:%d currentTrack:%d", 0, currentTrack);
				// if((audio_element_get_state(i2s_stream_writer)==AEL_STATE_RUNNING)&&(c->play_to_end==1)){
				// 	ESP_LOGD(TAG, "skip restart track");
				// }else
				{
					ESP_LOGD(TAG, "before shift:%d, cmd_arg = '%s'", me_state.currentTrack, cmd_arg);
					trackShift(cmd_arg);
					ESP_LOGD(TAG, "after shift:%d", me_state.currentTrack);
					vTaskDelay(pdMS_TO_TICKS(c->play_delay));
					if(audioPlay(c->handler, me_state.currentTrack)==ESP_OK){
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
					ESP_LOGD(TAG, "attenuation start");
				}else{
					//audioStop();
					//audioSetIndicator(slot_num, 0);
				}
				break;

            case MYCMD_shift:
				trackShift(cmd_arg);
				// if((audio_element_get_state(i2s_stream_writer)==AEL_STATE_RUNNING)){
				// 	if(c->play_to_end==0){
				// 		if(audioPlay(me_state.currentTrack)==ESP_OK){
				// 			audioSetIndicator(slot_num, 1);
				// 		}else{
				// 			audioSetIndicator(slot_num, 0);
				// 		}
				// 	}
				// }
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
				audioStop(c->handler);
				audioSetIndicator(slot_num, 0);
				att_vol = c->volume;
				att_flag=0;
			}
		}

		

		//listen audio event mp3_decoder
		// el_state = audio_element_get_state(mp3_decoder);
		// if(el_state == AEL_STATE_ERROR){
		// 	ESP_LOGE(TAG, "mp3_decoder Error state: %d", el_state);
		// 	esp_restart();
		// }

		// //listen audio event i2s_stream_writer
		// ret = audio_event_iface_listen(evt, &msg, 0);
		// if(ret == ESP_OK)
		// {
		//  	//ESP_LOGD(TAG, "audio_Event: %d el_state: %d", msg.cmd, el_state);
		// 	if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
		// 		el_state = audio_element_get_state(i2s_stream_writer);
		// 		if (el_state == AEL_STATE_FINISHED) {
		// 			audioStop();
		// 			audioSetIndicator(slot_num, 0);
		// 			//ESP_LOGD(TAG, "endOfTrack EVENT !!!!!!!!!!!!!!!!!");
		// 			vTaskDelay(pdMS_TO_TICKS(10));
		// 			memset(reportStr, 0, strlen(reportStr));
		// 			sprintf(reportStr,"%d", me_state.currentTrack);
		// 			//report(reportStr, slot_num);
		// 			stdreport_s(c->ETreport, reportStr);
		// 		}
		// 	}
		// 	if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) 
		// 	{
		// 		if (msg.source == (void *) mp3_decoder && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) 
		// 		{
		// 			audio_element_info_t music_info = {0};
		// 			audio_element_getinfo(mp3_decoder, &music_info);

		// 			ESP_LOGD(TAG, "Current track: sample_rates=%d, bits=%d, ch=%d", 
		// 					music_info.sample_rates, 
		// 					music_info.bits, 
		// 					music_info.channels);

		// 			// ESP_LOGI(TAG, "[ * ] Received music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
		// 			// 		music_info.sample_rates, music_info.bits, music_info.channels);
		// 			audio_element_setinfo(i2s_stream_writer, &music_info);
		// 			rsp_filter_set_src_info(rsp_handle, music_info.sample_rates, music_info.channels);
		// 		}
		// 	}
		// }

		wav_handle_turn(c->handler);
	}
}

void wavPlayerInit(uint8_t slot_num){
	//uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];

	strcpy(me_config.audioExtension, ".wav");

	sprintf(tmpString, "task_wavplayer_%d", slot_num);
	xTaskCreatePinnedToCore(wavplayer_task, tmpString, 1024*8, &t_slot_num,configMAX_PRIORITIES-5, NULL, 0);
}

void wavPlayerDeinit(PWAVPLAYERCONFIG c) {
  // that'll do pig... that'll do
  //i2s_del_channel(c->audio_ch_handle); // delete the channel

  wav_handle_deinit(c->handler);
}
static void trackShift(char* cmd_arg)
{
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

static void audioSetIndicator(uint8_t slot_num, uint32_t level){
	gpio_num_t led_pin = SLOTS_PIN_MAP[slot_num][3];
	gpio_set_level(led_pin, level);// module led light
}
static void setVolume_num(uint8_t vol) {
	if(vol>100){
		vol=100;
	}
	//i2s_alc_volume_set(i2s_stream_writer, -34 + (vol / 3));
}

static esp_err_t audioPlay(wav_handle_t h, uint8_t truckNum) 
{
	ESP_LOGD(TAG, "Playing file: %s", me_config.soundTracks[truckNum]);

	wav_handle_play(h, me_config.soundTracks[truckNum]);

	return ESP_OK;
}

static void audioStop(wav_handle_t h) 
{
	ESP_LOGD(TAG, "Stop playing. Free heap:%d", xPortGetFreeHeapSize());

	wav_handle_stop(h);
}

const char * get_manifest_wavPlayer()
{
	return manifesto;
}
