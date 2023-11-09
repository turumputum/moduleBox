#include "me_slot_config.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include "executor.h"
#include "stateConfig.h"
#include "include/buttons.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "reporter.h"
#include "audioPlayer.h"
#include "3n_mosfet.h"
#include "encoders.h"
#include "TOFs.h"
#include "tachometer.h"
#include "analog.h"
#include "esp_heap_caps.h"
#include "stepper.h"
#include "in_out.h"
#include "myMqtt.h"

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ME_SLOT_CONFIG";

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

extern configuration me_config;

//uint8_t SLOTS_PIN_MAP[4][3] = {{4,5,10},{17,18,0},{3,41,0},{2,1,0}}; //inty park solution, old boards v2.0
//uint8_t SLOTS_PIN_MAP[4][3] = {{4,5,10},{17,18,0},{6,7,8},{1,2,3}}; // v2.1 
//uint8_t SLOTS_PIN_MAP[6][3] = {{4,5,10},{17,18,15},{7,6,0},{3,8,0},{3,8,0},{41,42,0}}; //v2.2
uint8_t SLOTS_PIN_MAP[6][4] = {{4,5,10,38},{40,21,47,48},{17,18,15,0},{3,8,39,0},{2,1,41,0},{7,6,42,0}}; //v3.x



int init_slots(void){
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();

	for(int i=0;i<NUM_OF_SLOTS; i++){
		ESP_LOGD(TAG,"[%d] check mode:%s", i,me_config.slot_mode[i]);
		if(!memcmp(me_config.slot_mode[i], "audio_player_mono", 17)){
			audioInit();
		}else if(!memcmp(me_config.slot_mode[i], "button_optorelay", 16)){
			start_button_task(i);
			//init_optorelay(i);
		}else if(!memcmp(me_config.slot_mode[i], "button_led", 10)){
			start_button_task(i);
			init_led(i);
		}else if(!memcmp(me_config.slot_mode[i], "3n_mosfet", 9)){
			init_3n_mosfet(i);
		}else if(!memcmp(me_config.slot_mode[i], "encoderPWM", 10)){
			start_encoderPWM_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "encoder_inc", 10)){
			start_encoder_inc_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "benewake_TOF", 12)){
			start_benewakeTOF_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "tachometer", 10)){
			start_tachometer_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "analog", 6)){
			start_analog_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "stepper", 7)){
			start_stepper_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "in_out", 6)){
			init_out(i);
			start_in_task(i);
			
		}
	}

	ESP_LOGD(TAG, "Load config complite. Duration: %ld ms. Heap usage: %lu",
				(xTaskGetTickCount() - startTick) * portTICK_RATE_MS,
				heapBefore - xPortGetFreeHeapSize());
	return ESP_OK;
}

int get_option_int_val(int num_of_slot, char* string){
	char *ind_of_vol = strstr(me_config.slot_options[num_of_slot], string);
	char options_copy[strlen(ind_of_vol)];
	strcpy(options_copy, ind_of_vol);
	char *rest;
	char *ind_of_eqal=strstr(ind_of_vol, ":");
	if(ind_of_eqal!=NULL){
		if(strstr(ind_of_vol, ",")!=NULL){
			ind_of_vol = strtok_r(options_copy,",",&rest);
		}
		return atoi(ind_of_eqal+1);
	}else{
		ESP_LOGW(TAG, "Options wrong format:%s", ind_of_vol);
		free(options_copy);
		return -1;
	}
}

float get_option_float_val(int num_of_slot, char* string){
	char *ind_of_vol = strstr(me_config.slot_options[num_of_slot], string);
	char options_copy[strlen(ind_of_vol)];
	strcpy(options_copy, ind_of_vol);
	char *rest;
	char *ind_of_eqal=strstr(ind_of_vol, ":");
	if(ind_of_eqal!=NULL){
		if(strstr(ind_of_vol, ",")!=NULL){
			ind_of_vol = strtok_r(options_copy,",",&rest);
		}
		return atof(ind_of_eqal+1);
	}else{
		ESP_LOGW(TAG, "Options wrong format:%s", ind_of_vol);
		free(options_copy);
		return -1;
	}
}
char* get_option_string_val(int num_of_slot, char* option){
	char* resault;

	char *ind_of_vol = strstr(me_config.slot_options[num_of_slot], option);
	char options_copy[strlen(ind_of_vol)];
	strcpy(options_copy, ind_of_vol);
	char *rest;
	char *ind_of_eqal=strstr(ind_of_vol, ":");
	if(ind_of_eqal!=NULL){
		if(strstr(ind_of_vol, ",")!=NULL){
			ind_of_vol = strtok_r(ind_of_eqal,",",&rest);
		}else{
			ind_of_vol=ind_of_eqal;
		}
	}
	resault = malloc(strlen(ind_of_vol)*sizeof(char));
	resault = ind_of_vol+1;
	return resault;
	// }
}


// char* get_option_string_val(int num_of_slot, char* option, char* custom_topic){
// 	char* resault;

// 	char *ind_of_vol = strstr(me_config.slot_options[num_of_slot], option);
// 	char options_copy[strlen(ind_of_vol)];
// 	strcpy(options_copy, ind_of_vol);
// 	char *rest;
// 	char *ind_of_eqal=strstr(ind_of_vol, ":");
// 	if(ind_of_eqal!=NULL){
// 		if(strstr(ind_of_vol, ",")!=NULL){
// 			ind_of_vol = strtok_r(ind_of_eqal,",",&rest);
// 		}
// 	}
	
// 	return (ind_of_vol+1);
// 	// }
// }




