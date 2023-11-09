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

#include "stateConfig.h"

#include "board.h"

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "AUDIO";

audio_pipeline_handle_t pipeline;
audio_board_handle_t board_handle;
audio_element_handle_t i2s_stream_writer, mp3_decoder, fatfs_stream_reader, rsp_handle, equalizer;
char *url = NULL;
//playlist_operator_handle_t sdcard_list_handle = NULL;

audio_event_iface_handle_t evt;

extern stateStruct me_state;
extern configuration me_config;

void audioInit(void) {
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();
	//ESP_LOGD(TAG, "Start codec chip");

	if (strstr(me_config.slot_options[0], "volume")!=NULL){
		char *ind_of_vol = strstr(me_config.slot_options[0], "volume");
		char options_copy[strlen(ind_of_vol)];
		strcpy(options_copy, ind_of_vol);
		char *rest;
		char *ind_of_eqal=strstr(ind_of_vol, ":");
		if(ind_of_eqal!=NULL){
			if(strstr(ind_of_vol, ",")!=NULL){
				ind_of_vol = strtok_r(options_copy,",",&rest);
			}
			me_config.volume = atoi(ind_of_eqal+1);
			ESP_LOGD(TAG, "Set volume:%d", me_config.volume);
		}else{
			ESP_LOGW(TAG, "Volume options wrong format:%s", ind_of_vol);
		}
	}

	if (strstr(me_config.slot_options[0], "delay")!=NULL){
		char *ind_of_del = strstr(me_config.slot_options[0], "delay");
		char options_copy[strlen(ind_of_del)];
		strcpy(options_copy, ind_of_del);
		char *rest;
		char *ind_of_eqal=strstr(ind_of_del, ":");
		if(ind_of_eqal!=NULL){
			if(strstr(ind_of_del, ",")!=NULL){
				ind_of_del = strtok_r(options_copy,",",&rest);
			}
			me_config.play_delay = atoi(ind_of_eqal+1);
			ESP_LOGD(TAG, "Set play_delay:%d", me_config.play_delay);
		}else{
			ESP_LOGW(TAG, "Play_delay options wrong format:%s", ind_of_del);
		}
	}else{
		me_config.play_delay=0;
	}

	if (strstr(me_config.slot_options[0], "loop")!=NULL){
		me_config.loop=1;
		ESP_LOGD(TAG, "Set loop mode");
	}else{
		me_config.loop=0;
	}

	esp_rom_gpio_pad_select_gpio(38);
	gpio_set_direction(38, GPIO_MODE_OUTPUT);
	gpio_set_level(38, 0);

	board_handle = audio_board_init();
	audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

	//ESP_LOGD(TAG, "Create audio pipeline for playback");
	audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
	pipeline_cfg.rb_size = 8 * 1024;//3
	pipeline = audio_pipeline_init(&pipeline_cfg);
	mem_assert(pipeline);

	//ESP_LOGD(TAG, "Create i2s stream to write data to codec chip");
	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
	i2s_cfg.i2s_config.sample_rate = 48000;
	i2s_cfg.i2s_config.dma_buf_count = 3; //3
	i2s_cfg.i2s_config.dma_buf_len = 300; //300
	i2s_cfg.type = AUDIO_STREAM_WRITER;
	i2s_cfg.task_prio = 23; //23
	i2s_cfg.use_alc = true;
	i2s_cfg.volume = -34 + (me_config.volume / 3);
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

	xTaskCreatePinnedToCore(listenListener, "audio_listener", 1024 * 4, NULL, 1, NULL, 0);

	audio_pipeline_set_listener(pipeline, evt);

	ESP_LOGD(TAG, "Audio init complite. Duration: %ld ms. Heap usage: %lu free Heap:%u", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize(),
			xPortGetFreeHeapSize());
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

void audioPlay(char *cmd) {
	uint32_t heapBefore = xPortGetFreeHeapSize();

	if (cmd[0] == 43) {
		me_state.currentTrack += atoi(cmd + 1);
		if (me_state.currentTrack >= me_state.numOfTrack) {
			me_state.currentTrack = 0;
		}
		ESP_LOGD(TAG, "Current track index increment: %d", me_state.currentTrack);
	} else if (cmd[0] == 45) {
		me_state.currentTrack -= atoi(cmd + 1);
		if (me_state.currentTrack >= me_state.numOfTrack) {
			me_state.currentTrack = 0;
		}
		ESP_LOGD(TAG, "Current track index decrement: %d", me_state.currentTrack);
	} else if (cmd[0] == 35) {
		ESP_LOGD(TAG, "Play current truck:%d", me_state.currentTrack);
	}else if (strstr(cmd, "random")!=NULL) {
		me_state.currentTrack = rand() % me_state.numOfTrack;
		ESP_LOGD(TAG, "Set random track index: %d", me_state.currentTrack);
	}else if (strstr(cmd, "current")!=NULL) {
		ESP_LOGD(TAG, "Play current track index: %d", me_state.currentTrack);
	} else {
		me_state.currentTrack = atoi(cmd);
		if (me_state.currentTrack >= me_state.numOfTrack) {
			//ESP_LOGD(TAG, "currentTrack:%d numOfTrack:%d", me_state.currentTrack, me_state.numOfTrack);
			me_state.currentTrack = 0;
		}
		ESP_LOGD(TAG, "Current track index: %d of: %d ", me_state.currentTrack,me_state.numOfTrack);
	}

	audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
	if(el_state==AEL_STATE_RUNNING){
		audioStop();
	}

	vTaskDelay(pdMS_TO_TICKS(me_config.play_delay));

	audio_element_info_t music_info = { 0 };
	audio_element_set_uri(fatfs_stream_reader, me_config.soundTracks[me_state.currentTrack]);
	audio_element_getinfo(mp3_decoder, &music_info);
	ESP_LOGD(TAG, "Received music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d", music_info.sample_rates, music_info.bits, music_info.channels);
	setVolume_num(me_config.volume);
	audio_element_setinfo(i2s_stream_writer, &music_info);
	rsp_filter_set_src_info(rsp_handle, music_info.sample_rates, music_info.channels);
	//url = "/sdcard/monofonRus.mp3";
	//audio_hal_set_volume(board_handle->audio_hal, me_config.volume);
	//i2s_alc_volume_set(audio_element_handle_t i2s_stream, int volume);

	//ESP_LOGD(TAG, "Set volume: %d", me_config.volume);

	//audio_pipeline_reset_ringbuffer(pipeline);
	//audio_pipeline_reset_elements(pipeline);
	//audio_pipeline_change_state(pipeline, AEL_STATE_INIT);
	audio_pipeline_run(pipeline);

	ESP_LOGD(TAG, "Start playing file:%s Heap usage:%lu, Free heap:%u", me_config.soundTracks[me_state.currentTrack], heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
	
	gpio_set_level(38, 1);

	vTaskDelay(pdMS_TO_TICKS(10));
	char tmpStr[strlen(me_config.device_name)+strlen("/player_start")+10];
	sprintf(tmpStr,"%s/player_start:%d", me_config.device_name, me_state.currentTrack);
	report(tmpStr);
}

void audioShift(char *cmd){
	if (cmd[0] == 43) {
		me_state.currentTrack += atoi(cmd + 1);
		if (me_state.currentTrack >= me_state.numOfTrack) {
			me_state.currentTrack = 0;
		}
		ESP_LOGD(TAG, "Shift track. Current track index increment: %d", me_state.currentTrack);
	} else if (cmd[0] == 45) {
		me_state.currentTrack -= atoi(cmd + 1);
		if (me_state.currentTrack >= me_state.numOfTrack) {
			me_state.currentTrack = 0;
		}
		ESP_LOGD(TAG, "Shift track. Current track index decrement: %d", me_state.currentTrack);
	}else if (strstr(cmd, "random")!=NULL) {
		me_state.currentTrack = rand() % me_state.numOfTrack;
		ESP_LOGD(TAG, "Shift track. Set random track index: %d", me_state.currentTrack);
	}else {
		me_state.currentTrack = atoi(cmd);
		if (me_state.currentTrack >= me_state.numOfTrack) {
			me_state.currentTrack = 0;
		}
		ESP_LOGD(TAG, "Shift track. Current track index: %d of: %d ", me_state.currentTrack,me_state.numOfTrack);
	}

	if(audio_element_get_state(i2s_stream_writer)==AEL_STATE_RUNNING){
		audioPlay("#");
	}
}

void audioStop(void) {
	//audio_pipeline_pause(pipeline);
	//ESP_ERROR_CHECK(audio_pipeline_stop(pipeline));
	
	audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
	ESP_LOGD(TAG, "audioStop state: %d", el_state);
	if ((el_state != AEL_STATE_FINISHED) && (el_state != AEL_STATE_STOPPED)) {
		audio_pipeline_stop(pipeline);
		audio_pipeline_wait_for_stop(pipeline);


	}
	ESP_ERROR_CHECK(audio_pipeline_terminate(pipeline));
	ESP_ERROR_CHECK(audio_pipeline_reset_ringbuffer(pipeline));
	ESP_ERROR_CHECK(audio_pipeline_reset_elements(pipeline));
	ESP_ERROR_CHECK(audio_pipeline_change_state(pipeline, AEL_STATE_INIT));

	gpio_set_level(38, 0);

	ESP_LOGD(TAG, "Stop playing. Free heap:%d", xPortGetFreeHeapSize());
}

void audioPause(void) {

	ESP_LOGD(TAG, "Pausing audio pipeline");
	audio_pipeline_pause(pipeline);
}

void listenListener(void *pvParameters) {
	while (1) {
		

		audio_event_iface_msg_t msg;
		esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
		if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
			audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
			if (el_state == AEL_STATE_FINISHED) {
				audioStop();
				
				vTaskDelay(pdMS_TO_TICKS(10));

				char tmpStr[strlen(me_config.device_name)+strlen("/player_end")+10];
				sprintf(tmpStr,"%s/player_end:%d", me_config.device_name, me_state.currentTrack);
				report(tmpStr);
				
			}
		}

	}

}
