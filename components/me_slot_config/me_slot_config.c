#include "me_slot_config.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include "executor.h"
#include "stateConfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"

#include "esp_err.h"
#include "reporter.h"
#include "audioPlayer.h"
#include "wavPlayer.h"
#include "3n_MOSFET.h"
#include "encoders.h"
#include "tachometer.h"
#include "analog.h"
#include "esp_heap_caps.h"
#include "stepper.h"
#include "in_out.h"
#include "buttonLeds.h"
#include "myMqtt.h"
#include "sensor_2ch.h"
#include "tenzo_button.h"
#include "flywheel.h"
#include "virtualSlots.h"
#include "myHID.h"
#include "swiper.h"
#include "rfid.h"
#include "someUnique.h"
#include "buttonMatrix.h"
#include "dwin.h"

#include "distanceSens.h"
#include "lidars.h"
#include "mb_oneWire.h"
#include "accel.h"
#include "servoDev.h"
#include "steadywin.h"
#include "ticketDispenser.h"
#include "VESC.h"
#include "PPM.h"
#include "CRSF.h"
#include <stdarg.h>
#include <rgbHsv.h>
#include <stdreport.h>
#include "audioLAN.h"
#include "opusLAN.h"
#include "dialer.h"
#include <mbdebug.h>
#include <testsd.h>


#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ME_SLOT_CONFIG";


#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

extern stateStruct me_state;
extern configuration me_config;

//uint8_t SLOTS_PIN_MAP[4][3] = {{4,5,10},{17,18,0},{3,41,0},{2,1,0}}; //inty park solution, old boards v2.0
//uint8_t SLOTS_PIN_MAP[4][3] = {{4,5,10},{17,18,0},{6,7,8},{1,2,3}}; // v2.1 
//uint8_t SLOTS_PIN_MAP[10][3] = {{4,5,10},{17,18,15},{7,6,0},{3,8,0},{3,8,0},{41,42,0}}; //v2.2
//uint8_t SLOTS_PIN_MAP[10][4] = {{4,5,10,38},{40,21,47,48},{17,18,15,0},{3,8,39,0},{2,1,41,0},{7,6,42,0}}; //v3.x
uint8_t SLOTS_PIN_MAP[10][4];

const uint8_t PIN_MAP_v3[10][4] = {{4,5,10,38},{40,21,47,48},{17,18,15,0},{3,8,39,0},{2,1,41,0},{7,6,42,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0}};//v3.2x
const uint8_t PIN_MAP_v4[10][4] = {{1,7,17,47},{2,8,18,48},{6,10,21,0},{4,15,38,0},{5,39,42,0},{3,40,41,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0}};
const uint8_t PIN_MAP_v6[10][4] = {{5,4,6,0},{7,9,8,0},{1,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0}};

adc_channel_t SLOT_ADC_MAP[6]={
    ADC1_CHANNEL_3,
    ADC1_CHANNEL_0, // Placeholder for unused channel, will be checked in code
    ADC2_CHANNEL_6,
    ADC1_CHANNEL_2,
    ADC1_CHANNEL_1,
    ADC1_CHANNEL_6
};


int init_slots(void){
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();

	
#ifdef BOARD_PINOUT_V6
	memcpy(SLOTS_PIN_MAP, PIN_MAP_v6, sizeof(PIN_MAP_v6));
#else
	if(me_config.boardVersion==4){
		memcpy(SLOTS_PIN_MAP, PIN_MAP_v4, sizeof(PIN_MAP_v4));
	}else{
		memcpy(SLOTS_PIN_MAP, PIN_MAP_v3, sizeof(PIN_MAP_v3));
	}
#endif

	stdreport_initialize();
	reporter_init();

	for(int i=0;i<NUM_OF_SLOTS; i++){
		// Check if slot_mode[i] is not NULL before accessing it
		if(me_config.slot_mode[i] == NULL) {
			ESP_LOGD(TAG,"[%d] slot_mode is NULL, skipping", i);
			continue;
		}

		ESP_LOGD(TAG,"[%d] check mode: '%s'", i, me_config.slot_mode[i]);

		const char *mode = me_config.slot_mode[i];

		if(!strlen(mode) || !strcmp(mode, "empty") || !strcmp(mode, "SD_card")){
			// empty
		}else if(!strcmp(mode, "mp3Player")){
			audioInit(i);
		}else if(!strcmp(mode, "wavPlayer")){
			wavPlayerInit(i);
		}else if(!strcmp(mode, "audioLAN")){
			start_audioLAN_task(i); 		// W, C
		}else if(!strcmp(mode, "opusLAN")){
			start_opusLAN_task(i); 		// W, C (Opus codec)
		}else if(!strcmp(mode, "button_ledRing")){
			start_button_ledRing_task(i);
		}else if(!strcmp(mode, "button_runFire")){
			start_button_runFire_task(i);
		}else if(!strcmp(mode, "button_ledBar")){
			start_button_ledBar_task(i);
		}else if(!strcmp(mode, "button_swiperLed")){
			start_button_swiperLed_task(i);
		}else if(!strcmp(mode, "button_smartLed")){
			start_button_smartLed_task(i);
		}else if(!strcmp(mode, "button_led")){
			start_button_led_task(i);
		}else if(!strcmp(mode, "in_out")){
			start_in_out_task(i);
		}else if(!strcmp(mode, "in_2ch")){
			start_in_2ch_task(i);	// W, NOC
		}else if(!strcmp(mode, "in_3ch")){
			start_in_3ch_task(i);	// W, NOC
		}else if(!strcmp(mode, "out_2ch")){
			start_out_2ch_task(i);		// W, NOC
		}else if(!strcmp(mode, "relay")){
			start_relay_task(i);		// W, NOC
		}else if(!strcmp(mode, "pwmLeds")){
			init_pwmLeds(i);			// W, C
		}else if(!strcmp(mode, "encoderInc")){
			start_encoder_inc_task(i);	// W, C
		}else if(!strcmp(mode, "encoderAS5600")){
			start_encoderAS5600_task(i); // W, C
		}else if(!strcmp(mode, "benewakeTOF")){
			start_benewakeTOF_task(i);	// W, NOC
		}else if(!strcmp(mode, "TOFxxxF")){
			start_tofxxxfuart_task(i);		// W, NOC
		}else if(!strcmp(mode, "hlk2410")){
			start_hlk2410_task(i);		// W, NOC
		}else if(!strcmp(mode, "rplidarS1")){
			start_rplidarS1_task(i);	// W, NOC
		}else if(!strcmp(mode, "sr04m")){
			start_sr04m_task(i);	// W, NOC
		}else if(!strcmp(mode, "tachometer")){
			start_tachometer_task(i);	// W, NOC
		}else if(!strcmp(mode, "analog")){
			start_adc1_task(i); // W, C
		}else if(!strcmp(mode, "stepper")){
			start_stepper_task(i);		// W, C
		}else if(!strcmp(mode, "testsd")){
			start_testsd_task(i);		// W, C
		}else if(!strcmp(mode, "tenzoButton")){
			start_tenzo_button_task(i);	// W, NOC
		}else if(!strcmp(mode, "flywheel")){
			start_flywheel_task(i);		//OK, NOC
		}else if(!strcmp(mode, "scaler")){
			start_scaler_task(i);		// W, NOC
		}else if(!strcmp(mode, "swiper")){
			start_swiper_task(i);		// W, NOC
		}else if(!strcmp(mode, "rfid")){
			start_pn532Uart_task(i);	// W, NOC
		}else if(!strcmp(mode, "startup")){
			start_startup_task(i);		// NW, NOC
		}else if(!strcmp(mode, "HID")){
			start_HID_task(i);			// W, NOC
		}else if(!strcmp(mode, "counter")){
			start_counter_task(i);		// W, C
		}else if(!strcmp(mode, "timer")){
			start_timer_task(i);		// W, C
		}else if(!strcmp(mode, "watchdog")){
			start_watchdog_task(i);		// W, NOC
		}else if(!strcmp(mode, "uartLogger")){
			start_uartLogger_task(i);   // W, NOC
		}else if(!strcmp(mode, "buttonMatrix")){
			start_buttonMatrix_task(i);	// W, NOC
		}else if(!strcmp(mode, "dwin")){
			start_dwinUart_task(i);		// W, NOC
		}else if(!strcmp(mode, "dialer")){
			start_dialer_task(i);		// W, NOC
		}else if(!strcmp(mode, "whitelist")){
			start_whitelist_task(i);	// W, C
		}else if(!strcmp(mode, "collector")){
			start_collector_task(i);	// W, NOC
		}else if(!strcmp(mode, "masquerade")){
			start_masquerade_task(i);	// W, NOC
		}else if(!strcmp(mode, "random")){
			start_random_task(i);	// W, C
		}else if(!strcmp(mode, "ds18b20")){
			start_ds18b20_task(i);		// W, NOC
		}else if(!strcmp(mode, "steadywinGIM")){
			start_GIM_motor_task(i);	// W, NOC
		}else if(!strcmp(mode, "CRSF")){
			start_crsf_rx_task(i);		// W, NOC
		}else if(!strcmp(mode, "VESC")){
			start_CAN_VESC_task(i);		// ???
		}else if(!strcmp(mode, "PPM")){
			start_ppm_generator_task(i);// W, NOC
		}else if(!strcmp(mode, "tankControl")){
			start_tankControl_task(i);	// W, NOC
		}else if(!strcmp(mode, "furbyEye")){
			start_furbyEye_task(i); // W, NOC
		}else if(!strcmp(mode, "conductor")){
			start_conductor_task(i); // NOW, NOC
		}else if(!strcmp(mode, "st7789")){
			start_st7789_task(i);
		}else{
			mblog(E, "Wrong mode for SLOT_%d: %s", i, mode);
		}

	}

	ESP_LOGD(TAG, "Load config complite. Duration: %ld ms. Heap usage: %lu",
				(xTaskGetTickCount() - startTick) * portTICK_RATE_MS,
				heapBefore - xPortGetFreeHeapSize());
	return ESP_OK;
}

int get_option_flag_val(int slot_num, char* string)
{
	int  	result = 0;

	if (strstr(me_config.slot_options[slot_num], string) != NULL)
	{
		result = 1;
	}

	return result;
}
int get_option_int_val(int slot_num, char* string, const char*  unit_name, int default_value, int min_value, int max_value)
{
	int 		result	= default_value;
	char *		begin;
	char *		value;
	char *		end;
	int 		len;

	if ((begin = strstr(me_config.slot_options[slot_num], string)) != NULL)
	{
		if ((end = strchr(begin, ',')) == NULL)
		{ end = begin + strlen(begin); }
		len = end - begin;
		char dup [len + 1];
		memcpy(dup, begin, len);
		dup[len] = 0;

		if ((value = strchr(begin, ':')) != NULL)
		{
			value++;

			int parsed = atoi(value);
			if (parsed < min_value) {
				ESP_LOGW(TAG, "Option '%s'=%d below min=%d, clamped", string, parsed, min_value);
				parsed = min_value;
			} else if (parsed > max_value) {
				ESP_LOGW(TAG, "Option '%s'=%d above max=%d, clamped", string, parsed, max_value);
				parsed = max_value;
			}
			result = parsed;
		}
		else
		{
			ESP_LOGW(TAG, "Options wrong format:%s", dup);
		}
	}

	return result;
}

float get_option_float_val(int slot_num, char* string, float default_value)
{
	float 		result	= default_value;
	char *		begin;
	char *		value;
	char *		end;
	int 		len;

	if ((begin = strstr(me_config.slot_options[slot_num], string)) != NULL)
	{
		if ((end = strchr(begin, ',')) == NULL)
		{ end = begin + strlen(begin); }
		len = end - begin;
		char dup [len + 1];
		memcpy(dup, begin, len);
		dup[len] = 0;

		if ((value = strchr(begin, ':')) != NULL)
		{
			value++;

			result = atof(value);
		}
		else
		{
			ESP_LOGW(TAG, "Options wrong format:%s", dup);
		}
	}

	return result;
}
char* get_option_string_val(int slot_num, char* option, char* default_value){
	const char *options = me_config.slot_options[slot_num];
	if (options == NULL) {
		return default_value;
	}

	char *begin = strstr(options, option);
	if (begin == NULL) {
		return default_value;
	}

	char *colon = strchr(begin, ':');
	if (colon == NULL) {
		ESP_LOGW(TAG, "Options wrong format (no ':' for '%s')", option);
		return default_value;
	}
	const char *value = colon + 1;

	const char *end = strchr(value, ',');
	size_t len = (end != NULL) ? (size_t)(end - value) : strlen(value);

	char *result = heap_caps_malloc(len + 1, MALLOC_CAP_8BIT);
	if (result == NULL) {
		return default_value;
	}
	memcpy(result, value, len);
	result[len] = '\0';
	return result;
}
static char * _cleanValue(char *         value)
{
	char * 			result;
    unsigned char * on 		= (unsigned char *)value;

    while (*on && ((*on <= ' ') || (*on == '\"')))
    {
        on++;
    }

	result 	= (char*)on;
    int len = strlen(result);

    if (len)
    {
        result[len] = 0;
        
        on = (unsigned char*)&result[len - 1];
        
        // Clean end
        while ((on > (unsigned char*)result) && ((*on <= ' ') || (*on == '\"')))
        {
            on--;
        }
        
        *(on + 1) = 0;
    }

	return result;
}
int get_option_enum_val(int slot_num, char* option, ...)
{
	int 			result			= -1;
    va_list         list;
	char * 			arg;
	char *			begin;
	char *			value;
	char *			end;
	int 			len;

    va_start(list, option);

	if ((begin = strstr(me_config.slot_options[slot_num], option)) != NULL)
	{
		if ((end = strchr(begin, ',')) == NULL)
		{ end = begin + strlen(begin); }
		len = end - begin;
		char dup [len + 1];
		memcpy(dup, begin, len);
		dup[len] = 0;

		if ((value = strchr(dup, ':')) != NULL)
		{
			value++;
			value = _cleanValue(value);

			if (strlen(value) > 0)
			{
				for (int idx = 0; ((result < 0) && ((arg = va_arg(list, char *)) != NULL)); idx++)
				{
					if (!strcmp(arg, value))
					{
						result = idx;
					}
				}
			}
		}
		else
		{
			ESP_LOGW(TAG, "Options wrong format:%s", dup);
		}
	}
	else
		result = 0;

	va_end(list);

	return result;
}

int get_option_color_val(RgbColor * output, int slot_num, char* string, char * default_value)
{
	int 		result	= ESP_FAIL;
	int 		found	= 0;
	char *		begin;
	char *		value;
	char *		end;
	int 		len;

	if ((begin = strstr(me_config.slot_options[slot_num], string)) != NULL)
	{
		found = 1;
		if ((end = strchr(begin, ',')) == NULL)
		{ end = begin + strlen(begin); }
		len = end - begin;
		char dup [len + 1];
		memcpy(dup, begin, len);
		dup[len] = 0;

		if ((value = strchr(begin, ':')) != NULL)
		{
			value++;

			if (parseRGB(output, value) == -1)
			{
				ESP_LOGW(TAG, "Color wrong format:%s", dup);
			}
			else
				result = ESP_OK;
		}
		else
		{
			ESP_LOGW(TAG, "Options wrong format:%s", dup);
		}
	}

	if (result != ESP_OK)
	{
		ESP_LOGW(TAG, "Color options not found, lets parse default:%s", default_value);
		parseRGB(output, default_value);
		// Опция не задана - это норма- дефолт применён, ошибкой не считаем-
		// только реально кривое значение (found && !OK) оставляем как ESP_FAIL-
		if (!found) result = ESP_OK;
	}

	return result;
}

int get_next_ledc_channel(void)
{
	if (me_state.ledc_chennelCounter >= 8)
	{
		char * tmpStr = heap_caps_malloc(128, MALLOC_CAP_8BIT);
		if (tmpStr != NULL) {
			sprintf(tmpStr, "LEDC channel limit reached: %d", me_state.ledc_chennelCounter);
			mblog(E, tmpStr);

			sprintf(tmpStr, "%s/system/error:LEDC channel limit reached", me_config.deviceName);
			report(tmpStr, 0);
			heap_caps_free(tmpStr);
		}

		return -1;
	}
	return me_state.ledc_chennelCounter++;
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
