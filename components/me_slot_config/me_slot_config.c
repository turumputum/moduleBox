#include "me_slot_config.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include "executor.h"
#include "stateConfig.h"
#include "buttonLed.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"

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
#include "smartLed.h"
#include "myMqtt.h"
#include "sensor_2ch.h"
#include "tenzo_button.h"
#include "flywheel.h"
#include "virtual_slots.h"
#include "myHID.h"
#include "swiper.h"
#include "rfid.h"
#include "disp_hd44780.h"
#include "someUnique.h"
#include "max7219_task.h"
#include "hlk_sens.h"
#include "dwin_uart.h"

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ME_SLOT_CONFIG";


#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

extern stateStruct me_state;
extern configuration me_config;

//uint8_t SLOTS_PIN_MAP[4][3] = {{4,5,10},{17,18,0},{3,41,0},{2,1,0}}; //inty park solution, old boards v2.0
//uint8_t SLOTS_PIN_MAP[4][3] = {{4,5,10},{17,18,0},{6,7,8},{1,2,3}}; // v2.1 
//uint8_t SLOTS_PIN_MAP[10][3] = {{4,5,10},{17,18,15},{7,6,0},{3,8,0},{3,8,0},{41,42,0}}; //v2.2
//uint8_t SLOTS_PIN_MAP[10][4] = {{4,5,10,38},{40,21,47,48},{17,18,15,0},{3,8,39,0},{2,1,41,0},{7,6,42,0}}; //v3.x
uint8_t SLOTS_PIN_MAP[10][4] = {{4,5,10,38},{40,21,47,48},{17,18,15,0},{3,8,39,0},{2,1,41,0},{7,6,42,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0}}; //v3.2x

adc_channel_t SLOT_ADC_MAP[6]={
    ADC1_CHANNEL_3,
    -1,
    ADC2_CHANNEL_6,
    ADC1_CHANNEL_2,
    ADC1_CHANNEL_1,
    ADC1_CHANNEL_6
};


int init_slots(void){
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();
	reporter_init();

	for(int i=0;i<NUM_OF_SLOTS; i++){
		ESP_LOGD(TAG,"[%d] check mode:%s", i,me_config.slot_mode[i]);
		if(!memcmp(me_config.slot_mode[i], "audio_player", 12)){
			audioInit(i);
		}else if(!memcmp(me_config.slot_mode[i], "button_ledRing", 14)){
			//start_button_task(i);
			start_ledRing_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "button_led", 10)){
			start_button_task(i);
			start_led_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "button_smartLed", 15)){
			start_button_task(i);
			start_smartLed_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "sensor_2ch", 9)){
			start_sensors_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "pwmRGBled", 9)){
			init_3n_mosfet(i);
		}else if(!memcmp(me_config.slot_mode[i], "encoder_PWM", 10)){
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
			start_out_task(i);
			start_in_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "tenzoButton", 12)){
			start_tenzo_button_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "flywheel", 7)){
			start_flywheel_task(i);
			start_led_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "swiper", 6)){
			start_swiper_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "rfid", 4)){
			start_pn532Uart_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "startup", 8)){
			start_startup_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "HID", 3)){
			start_HID_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "counter", 3)){
			start_counter_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "timer", 3)){
			start_timer_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "watchdog", 3)){
			start_watchdog_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "uartLogger", 3)){
			start_uartLogger_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "disp_hd44780", 12)){
			start_disp_hd44780_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "disp_max7219", 12)){
			start_max7219_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "buttonMatrix4", 13)){
			start_buttonMatrix4_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "dwin_uart", 9)){
			start_dwinUart_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "hlk2420", 7)){
			start_hlk2420_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "hlk2410", 7)){
			start_hlk2410_task(i);
		}else if(!memcmp(me_config.slot_mode[i], "dialer", 7)){
			start_dialer_task(i);
		}
	}

	ESP_LOGD(TAG, "Load config complite. Duration: %ld ms. Heap usage: %lu",
				(xTaskGetTickCount() - startTick) * portTICK_RATE_MS,
				heapBefore - xPortGetFreeHeapSize());
	return ESP_OK;
}

int get_option_int_val(int slot_num, char* string){
	char *ind_of_vol = strstr(me_config.slot_options[slot_num], string);
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
		//free(options_copy);
		return -1;
	}
}

float get_option_float_val(int slot_num, char* string){
	char *ind_of_vol = strstr(me_config.slot_options[slot_num], string);
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
char* get_option_string_val(int slot_num, char* option){
	char* resault;
	char *options_copy = strdup(me_config.slot_options[slot_num]);
	char *ind_of_vol = strstr(options_copy, option);
	//char options_copy[strlen(ind_of_vol)];
	//strcpy(options_copy, ind_of_vol);
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


// char* get_option_string_val(int slot_num, char* option, char* custom_topic){
// 	char* resault;

// 	char *ind_of_vol = strstr(me_config.slot_options[slot_num], option);
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




