#include <stdio.h>
#include "audioPlayer.h"

#include "esp_log.h"


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

#include "audio_event_iface.h"
#include "periph_wifi.h"

#include "reporter.h"
#include "executor.h"

#include "stateConfig.h"
#include "me_slot_config.h"

#include "board.h"

#include "esp_audio.h"

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
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

void trackShift(char* cmd_arg, int16_t *truckNum){
	//ESP_LOGD(TAG,"trackShift arg:%d", cmd_arg[0]);
	if (cmd_arg[0] == 43) {
		*truckNum += atoi(cmd_arg + 1);
		if (*truckNum >= me_state.numOfTrack) {
			*truckNum = 0;
		}
		ESP_LOGD(TAG, "Shift track. Current track index increment: %d", *truckNum);
	} else if (cmd_arg[0] == 45) {
		*truckNum -= atoi(cmd_arg + 1);
		if (*truckNum < 0) {
			*truckNum = me_state.numOfTrack-1;
		}
		ESP_LOGD(TAG, "Shift track. Current track index decrement: %d", *truckNum);
	} else if (cmd_arg[0] == 35) {
		ESP_LOGD(TAG, "noShift track. Current track index: %d", me_state.currentTrack);
	}else if (strstr(cmd_arg, "random")!=NULL) {
		*truckNum = rand() % me_state.numOfTrack;
		ESP_LOGD(TAG, "Shift track. Set random track index: %d", *truckNum);
	}else {
		*truckNum = atoi(cmd_arg);
		if (*truckNum >= me_state.numOfTrack) {
			*truckNum = 0;
		}
		ESP_LOGD(TAG, "Shift track. Current track index: %d of: %d ", *truckNum, me_state.numOfTrack);
	}
}

void audio_task(void *arg) {
	int slot_num = *(int*) arg;
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();

	gpio_num_t led_pin = SLOTS_PIN_MAP[slot_num][3];
	//ESP_LOGD(TAG, "Start codec chip");

	int16_t currentTrack=0;

	uint8_t volume=70;
	if (strstr(me_config.slot_options[0], "volume")!=NULL){
		float tmp = get_option_float_val(slot_num, "volume");
		volume = (int)(100*tmp);
		ESP_LOGD(TAG, "Set volume:%d", volume);
	}

	int attenuation=0;
	if (strstr(me_config.slot_options[0], "attenuation")!=NULL){
		attenuation = 1;
		if (attenuation != 0) {
			ESP_LOGD(TAG, "Enable attenuation");
		}
	}
	

	uint16_t play_delay=0;
	if (strstr(me_config.slot_options[0], "play_delay_ms")!=NULL){
		play_delay = get_option_int_val(slot_num, "play_delay_ms");
		ESP_LOGD(TAG, "Set play_delay:%d", play_delay);
	}

	uint8_t play_to_end=0;
	if (strstr(me_config.slot_options[0], "play_to_end")!=NULL){
		play_to_end = 1;
		ESP_LOGD(TAG, "Set play_to_end:%d", play_to_end);
	}

	//---add action to topic list---
	if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/player_")+3];
		sprintf(t_str, "%s/player_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	}

	esp_rom_gpio_pad_select_gpio(led_pin);
	gpio_set_direction(led_pin, GPIO_MODE_OUTPUT);
	gpio_set_level(led_pin, 0);

	board_handle = audio_board_init();// TO_DO raspetrushit'
	audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

	//ESP_LOGD(TAG, "Create audio pipeline for playback");
	audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
	pipeline_cfg.rb_size = 8 * 1024;//3
	pipeline = audio_pipeline_init(&pipeline_cfg);
	mem_assert(pipeline);

	//ESP_LOGD(TAG, "Create i2s stream to write data to codec chip");
	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
	// i2s_cfg.i2s_config.sample_rate = 48000;
	// i2s_cfg.i2s_config.dma_buf_count = 3; //3
	// i2s_cfg.i2s_config.dma_buf_len = 300; //300
	i2s_cfg.type = AUDIO_STREAM_WRITER;
	i2s_cfg.task_prio = 23; //23
	i2s_cfg.use_alc = true;
	i2s_cfg.volume = -34 + (volume / 3);
	i2s_stream_writer = i2s_stream_init(&i2s_cfg);

	fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
	fatfs_cfg.type = AUDIO_STREAM_READER;
	fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);

	//ESP_LOGD(TAG, "Create mp3 decoder to decode mp3 file");
	mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
	//mp3_cfg.out_rb_size = 3 * 1024;
	mp3_decoder = mp3_decoder_init(&mp3_cfg);

	//ESP_LOGD(TAG, "Create resample filter");
	rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
	rsp_cfg.prefer_flag = 1;
	rsp_handle = rsp_filter_init(&rsp_cfg);

	//ESP_LOGD(TAG, "Register all elements to audio pipeline");
	audio_pipeline_register(pipeline, fatfs_stream_reader, "file");
	audio_pipeline_register(pipeline, mp3_decoder, "mp3");
	audio_pipeline_register(pipeline, rsp_handle, "filter");

	audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

	//ESP_LOGD(TAG, "Link it together [sdcard]-->fatfs_stream-->mp3_decoder-->resample-->i2s_stream-->[codec_chip]");
	const char *link_tag[4] = { "file", "mp3", "filter", "i2s" };
	audio_pipeline_link(pipeline, &link_tag[0], 4);

	//ESP_LOGD(TAG, "Set up  event listener");
	audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
	evt_cfg.external_queue_size = 10;//5
	evt_cfg.internal_queue_size = 10;//5
	evt_cfg.queue_set_size = 10;//5
	evt = audio_event_iface_init(&evt_cfg);

	//xTaskCreatePinnedToCore(listenListener, "audio_listener", 1024 * 4, NULL, 1, NULL, 0);
	audio_pipeline_set_listener(pipeline, evt);
	ESP_LOGD(TAG, "Audio init complite. Duration: %ld ms. Heap usage: %lu free Heap:%u", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize(),
			xPortGetFreeHeapSize());

	me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

	int att_flag=0;
	uint8_t att_vol=volume;
	command_message_t cmd;

	char reportStr[strlen("/endOfTrack")+10];
	//listen audio event
	audio_event_iface_msg_t msg;
	esp_err_t ret;
	audio_element_state_t el_state;

	while(1){
		//vTaskDelay(pdMS_TO_TICKS(30));
		
		if (xQueueReceive(me_state.command_queue[slot_num], &cmd, pdMS_TO_TICKS(30)) == pdPASS){
			char *command=cmd.str+strlen(me_state.action_topic_list[slot_num])+1;
			char *cmd_arg = NULL;
			if(command[0]=='/'){
				command = command+1;
			}
			if(strstr(command, ":")!=NULL){
				cmd_arg = strstr(command, ":")+1;
			}else{
				cmd_arg = strdup("0");
			}
			ESP_LOGD(TAG, "Incoming command:%s  arg:%s", command, cmd_arg); 
			if(!memcmp(command, "play", 4)){//------------------------------
				//ESP_LOGD(TAG, "AEL status:%d currentTrack:%d", audio_element_get_state(i2s_stream_writer), currentTrack);
				trackShift(cmd_arg, &currentTrack);
				if((audio_element_get_state(i2s_stream_writer)==AEL_STATE_RUNNING)&&(play_to_end==1)){
					ESP_LOGD(TAG, "skip restart track");
				}else{
					vTaskDelay(pdMS_TO_TICKS(play_delay));
					if(audioPlay(currentTrack)==ESP_OK){
						audioSetIndicator(slot_num, 1);
					}else{
						audioSetIndicator(slot_num, 0);
					}
					setVolume_num(volume);
				}
			}else if(!memcmp(command, "stop", 4)){//------------------------------
				if(attenuation!=0){
					att_flag=1;
					//ESP_LOGD(TAG, "attenuation start");
				}else{
					audioStop();
					audioSetIndicator(slot_num, 0);
				}
			}else if(!memcmp(command, "shift", 5)){//------------------------------
				trackShift(cmd_arg, &currentTrack);
				if((audio_element_get_state(i2s_stream_writer)==AEL_STATE_RUNNING)){
					if(play_to_end==0){
						if(audioPlay(currentTrack)==ESP_OK){
							audioSetIndicator(slot_num, 1);
						}else{
							audioSetIndicator(slot_num, 0);
						}
					}
				}
			}else if(!memcmp(command, "setVolume", 9)){//------------------------------
				setVolume_str(cmd_arg);
			}
		}

		if(att_flag==1){
			att_vol-=1;
			//ESP_LOGD(TAG, "attenuation vol:%d", att_vol);
			setVolume_num(att_vol);
			if(att_vol==0){
				audioStop();
				audioSetIndicator(slot_num, 0);
				att_vol = volume;
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
		el_state = audio_element_get_state(i2s_stream_writer);
		// if(ret == ESP_OK){
		// 	ESP_LOGD(TAG, "audio_Event: %d el_state: %d", msg.cmd, el_state);
		// }
		if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
			if (el_state == AEL_STATE_FINISHED) {
				audioStop();
				audioSetIndicator(slot_num, 0);
				//ESP_LOGD(TAG, "endOfTrack EVENT !!!!!!!!!!!!!!!!!");
				vTaskDelay(pdMS_TO_TICKS(10));
				memset(reportStr, 0, strlen(reportStr));
				sprintf(reportStr,"/endOfTrack:%d", me_state.currentTrack);
				report(reportStr, slot_num);
			}
		}
	}
}

void audioInit(uint8_t slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_player_%d", slot_num);
	xTaskCreatePinnedToCore(audio_task, tmpString, 1024*4, &t_slot_num,12, NULL, 1);
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
	uint32_t heapBefore = xPortGetFreeHeapSize();

	audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
	if(el_state==AEL_STATE_RUNNING){
		audioStop();
	}

	
	audio_element_info_t music_info = { 0 };
	audio_element_set_uri(fatfs_stream_reader, me_config.soundTracks[truckNum]);
	audio_element_getinfo(mp3_decoder, &music_info);
	//audio_element_getdata(mp3_decoder);
	// music_info.byte_pos=music_info.total_bytes/2;
	// audio_element_set_byte_pos(mp3_decoder, music_info.byte_pos);
	ESP_LOGD(TAG, "Received music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d byte_pos:%lld total_bytes:%lld", music_info.sample_rates, music_info.bits, music_info.channels, music_info.byte_pos, music_info.total_bytes);
	audio_element_setinfo(i2s_stream_writer, &music_info);
	rsp_filter_set_src_info(rsp_handle, music_info.sample_rates, music_info.channels);

	return audio_pipeline_run(pipeline);

	ESP_LOGD(TAG, "Start playing file:%s Heap usage:%lu, Free heap:%u", me_config.soundTracks[me_state.currentTrack], heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

void audioStop(void) {
	audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
	//ESP_LOGD(TAG, "audioStop state: %d", el_state);
	if ((el_state != AEL_STATE_FINISHED) && (el_state != AEL_STATE_STOPPED)) {
		audio_pipeline_stop(pipeline);
		audio_pipeline_wait_for_stop(pipeline);
	}
	ESP_ERROR_CHECK(audio_pipeline_terminate(pipeline));
	ESP_ERROR_CHECK(audio_pipeline_reset_ringbuffer(pipeline));
	ESP_ERROR_CHECK(audio_pipeline_reset_elements(pipeline));
	ESP_ERROR_CHECK(audio_pipeline_change_state(pipeline, AEL_STATE_INIT));

	ESP_LOGD(TAG, "Stop playing. Free heap:%d", xPortGetFreeHeapSize());
}

void audioPause(void) {

	ESP_LOGD(TAG, "Pausing audio pipeline");
	audio_pipeline_pause(pipeline);
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
