#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/timer.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/portmacro.h"
#include <driver/mcpwm.h>
#include "stateConfig.h"
#include "me_slot_config.h"
#include "reporter.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern uint8_t led_segment;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ANALOG";

void analog_task(void *arg)
{
    uint16_t raw_val;
    uint16_t resault=0, prev_resault=0;

	char tmpString[255];

    int slot_num = *(int *)arg;
	uint8_t sens_pin_num = SLOTS_PIN_MAP[slot_num][0];
    if(sens_pin_num>10){
        ESP_LOGE(TAG, "Wrong analog pin, chose another slot, task exit");
        vTaskDelete(NULL);
    }

	static const adc_bits_width_t width = ADC_WIDTH_BIT_12;
	static const adc_atten_t atten = ADC_ATTEN_DB_11; // ADC_ATTEN_DB_11

	gpio_reset_pin(sens_pin_num);
	gpio_set_direction(sens_pin_num, GPIO_MODE_INPUT);
    adc_channel_t ADC_chan= sens_pin_num -1;

    adc1_config_width(width);
    adc1_config_channel_atten(ADC_chan, atten);

    uint16_t MIN_VAL = 0;
    uint16_t MAX_VAL = 4095;
    uint8_t flag_float_output=0;
    if (strstr(me_config.slot_options[slot_num], "float_output")!=NULL){
		flag_float_output = 1;
		ESP_LOGD(TAG, "Set float output. Slot:%d", slot_num);
	}
	if (strstr(me_config.slot_options[slot_num], "max_val")!=NULL){
		MAX_VAL = get_option_int_val(slot_num, "max_val");
		ESP_LOGD(TAG, "Set max_val:%d. Slot:%d", MAX_VAL, slot_num);
	}
    if (strstr(me_config.slot_options[slot_num], "min_val")!=NULL){
		MIN_VAL = get_option_int_val(slot_num, "min_val");
		ESP_LOGD(TAG, "Set min_val:%d. Slot:%d", MIN_VAL, slot_num);
	}

    uint8_t inverse  = 0;
	if (strstr(me_config.slot_options[slot_num], "inverse")!=NULL){
		inverse=1;
	}

    float k=1;
    if (strstr(me_config.slot_options[slot_num], "filter_k")!=NULL){
        k = get_option_float_val(slot_num, "filter_k");
		ESP_LOGD(TAG, "Set k filter:%f.  Slot:%d", k, slot_num);
	}
    
    uint16_t dead_band=10;
    if (strstr(me_config.slot_options[slot_num], "dead_band")!=NULL){
        dead_band = get_option_int_val(slot_num, "dead_band");
		ESP_LOGD(TAG, "Set dead_band:%d. Slot:%d",dead_band, slot_num);
	}

	uint16_t periodic=0;
    if (strstr(me_config.slot_options[slot_num], "periodic")!=NULL){
        periodic = get_option_int_val(slot_num, "periodic");
		ESP_LOGD(TAG, "Set periodic:%d. Slot:%d",periodic, slot_num);
	}

    uint8_t flag_custom_topic = 0;
	char *custom_topic=NULL;
	if (strstr(me_config.slot_options[slot_num], "custom_topic")!=NULL){
		custom_topic = get_option_string_val(slot_num,"custom_topic");
		ESP_LOGD(TAG, "Custom topic:%s", custom_topic);
		flag_custom_topic=1;
	}

    if(flag_custom_topic==0){
		char *str = calloc(strlen(me_config.device_name)+strlen("/analog_")+4, sizeof(char));
		sprintf(str, "%s/analog_%d",me_config.device_name, slot_num);
		me_state.trigger_topic_list[slot_num]=str;
	}else{
		me_state.trigger_topic_list[slot_num]=custom_topic;
	}



    while (1) {
        if(inverse){
            raw_val = 4096-adc1_get_raw(ADC_chan);
        }else{
			raw_val = adc1_get_raw(ADC_chan);
        }
        resault =resault*(1-k)+raw_val*k;


        if((abs(resault - prev_resault)>dead_band)||(periodic!=0)){
            prev_resault = resault;
            //ESP_LOGD(TAG, "analog val:%d , allow_delta:%d", resault, MAX_VAL-MIN_VAL);

			int str_len;//=strlen(me_config.device_name)+strlen("/tachometer_")+8;
			char *str;// = (char*)malloc(str_len * sizeof(char));

            float f_res;
            if(flag_float_output){
				f_res = resault;
				if(f_res>MAX_VAL)f_res=MAX_VAL;
				if(f_res<MIN_VAL)f_res=MIN_VAL;
				f_res-=MIN_VAL;
				f_res = (float)f_res/(MAX_VAL-MIN_VAL);
            }

			memset(tmpString, 0, strlen(tmpString));

            if(flag_float_output){
				//sprintf(str,"%s/analog_%d:%f", me_config.device_name, slot_num, f_res);
				sprintf(tmpString,"%f", f_res);
			}else{
				//sprintf(str,"%s/analog_%d:%d", me_config.device_name, slot_num, resault);
				sprintf(tmpString,"%d", resault);
			}

			report(tmpString, slot_num);
			//free(str); 

        }
        //ESP_LOGD(TAG, "analog val:%d", resault);
        if(periodic!=0){
			vTaskDelay(pdMS_TO_TICKS(periodic));
		}else{
			vTaskDelay(pdMS_TO_TICKS(20));
		}
    }
    
}

void start_analog_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	// int slot_num = *(int*) arg;
	
	xTaskCreate(analog_task, "analog_task", 1024 * 4, &t_slot_num, 12, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "analog_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}