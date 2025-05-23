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
#include "someUnique.h"
#include "dwin.h"

#include "distanceSens.h"
#include "mb_oneWire.h"
#include "accel.h"
#include "servoDev.h"
#include "steadywin.h"
#include "ticketDispenser.h"
#include "VESC.h"
#include "PPM.h"
#include "CRSF.h"

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
		if(!memcmp(me_config.slot_mode[i], "audioPlayer", 11)){
			audioInit(i);
		}else if(!memcmp(me_config.slot_mode[i], "button_ledRing", 14)){
			start_button_task(i); 		// OK
			start_ledRing_task(i); 		// OK
		}else if(!memcmp(me_config.slot_mode[i], "button_ledBar", 13)){
			start_button_task(i); 		// OK
			start_ledBar_task(i);  		// OK
		}else if(!memcmp(me_config.slot_mode[i], "button_swiperLed", 16)){
			start_button_task(i);		// OK
			start_swiperLed_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "button_smartLed", 15)){
			start_button_task(i); 		// OK
			start_smartLed_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "button_led", 10)){
			start_button_task(i);		// OK
			start_led_task(i);			// OK
		}else if(!memcmp(me_config.slot_mode[i], "in_3ch", 6)){
			start_in_3ch_task(i, 3);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "in_2ch", 6)){
			start_in_3ch_task(i, 2);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "in_1ch", 6)){
			start_in_3ch_task(i, 1);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "pwmRGB", 6)){
			init_3n_mosfet(i);			// OK
		}else if(!memcmp(me_config.slot_mode[i], "encoderPPM", 10)){
			start_encoderPPM_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "encoderInc", 10)){
			start_encoder_inc_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "benewakeTOF", 12)){
			start_benewakeTOF_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "VL53TOF", 7)){
			start_VL53TOF_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "hlk2410", 7)){
			start_hlk2410_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "sr04m", 5)){
			start_ultrasonic_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "tachometer", 10)){
			start_tachometer_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "analog", 6)){
			start_analog_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "stepperSpeed", 12)){
			start_stepperSpeed_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "stepper", 7)){
			start_stepper_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "in_out", 6)){
			start_out_task(i);			// OK
			start_in_task(i);			// OK
		}else if(!memcmp(me_config.slot_mode[i], "out_3ch", 7)){
			start_out_3ch_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "out_2ch", 7)){
			start_out_2ch_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "tenzoButton", 12)){
			start_tenzo_button_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "flywheel", 7)){
			start_flywheel_task(i);		//OK
		}else if(!memcmp(me_config.slot_mode[i], "scaler", 6)){
			start_scaler_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "swiper", 6)){
			start_swiper_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "rfid", 4)){
			start_pn532Uart_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "startup", 8)){
			start_startup_task(i);		// ???
		}else if(!memcmp(me_config.slot_mode[i], "HID", 3)){
			start_HID_task(i);			// OK
		}else if(!memcmp(me_config.slot_mode[i], "counter", 3)){
			start_counter_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "timer", 3)){
			start_timer_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "watchdog", 3)){
			start_watchdog_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "uartLogger", 3)){
			start_uartLogger_task(i);   // OK
		}else if(!memcmp(me_config.slot_mode[i], "buttonMatrix", 13)){
			start_buttonMatrix_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "dwin", 4)){
			start_dwinUart_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "dialer", 7)){
			start_dialer_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "whitelist", 9)){
			start_whitelist_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "collector", 9)){
			start_collector_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "ds18b20", 7)){
			start_ds18b20_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "servoRod", 9)){
			start_servoRod_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "steadywinGIM", 12)){
			start_GIM_motor_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "ticketDispenser", 15)){
			start_ticketDispenser_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "volnaKolya", 10)){
			start_volnaKolya_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "CRSF", 4)){
			start_crsf_rx_task(i);		// OK
		}else if(!memcmp(me_config.slot_mode[i], "VESC", 4)){
			start_CAN_VESC_task(i);		// ???
		}else if(!memcmp(me_config.slot_mode[i], "PPM", 3)){
			start_ppm_generator_task(i);// OK
		}else if(!memcmp(me_config.slot_mode[i], "tankControl", 11)){
			start_tankControl_task(i);	// OK
		}else if(!memcmp(me_config.slot_mode[i], "furbyEye", 8)){
			start_furbyEye_task(i);		// OK
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




