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

extern uint8_t SLOTS_PIN_MAP[6][4];
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

    int num_of_slot = *(int *)arg;
	uint8_t sens_pin_num = SLOTS_PIN_MAP[num_of_slot][0];
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
    if (strstr(me_config.slot_options[num_of_slot], "float_output")!=NULL){
		flag_float_output = 1;
		ESP_LOGD(TAG, "Set float output. Slot:%d", num_of_slot);
	}
	if (strstr(me_config.slot_options[num_of_slot], "max_val")!=NULL){
		MAX_VAL = get_option_int_val(num_of_slot, "max_val");
		ESP_LOGD(TAG, "Set max_val:%d. Slot:%d", MAX_VAL, num_of_slot);
	}
    if (strstr(me_config.slot_options[num_of_slot], "min_val")!=NULL){
		MIN_VAL = get_option_int_val(num_of_slot, "min_val");
		ESP_LOGD(TAG, "Set min_val:%d. Slot:%d", MIN_VAL, num_of_slot);
	}

    uint8_t inverse  = 0;
	if (strstr(me_config.slot_options[num_of_slot], "inverse")!=NULL){
		inverse=1;
	}

    float k=1;
    if (strstr(me_config.slot_options[num_of_slot], "filter_k")!=NULL){
        k = get_option_float_val(num_of_slot, "filter_k");
		ESP_LOGD(TAG, "Set k filter:%f.  Slot:%d", k, num_of_slot);
	}
    
    uint16_t dead_band=10;
    if (strstr(me_config.slot_options[num_of_slot], "dead_band")!=NULL){
        dead_band = get_option_int_val(num_of_slot, "dead_band");
		ESP_LOGD(TAG, "Set dead_band:%d. Slot:%d",dead_band, num_of_slot);
	}

    uint8_t flag_custom_topic = 0;
	char *custom_topic=NULL;
	if (strstr(me_config.slot_options[num_of_slot], "custom_topic")!=NULL){
		custom_topic = get_option_string_val(num_of_slot,"custom_topic");
		ESP_LOGD(TAG, "Custom topic:%s", custom_topic);
		flag_custom_topic=1;
	}

    if(flag_custom_topic==0){
		char *str = calloc(strlen(me_config.device_name)+strlen("/analog_")+4, sizeof(char));
		sprintf(str, "%s/analog_%d",me_config.device_name, num_of_slot);
		me_state.triggers_topic_list[me_state.triggers_topic_list_index]=str;
	}else{
		me_state.triggers_topic_list[me_state.triggers_topic_list_index]=custom_topic;
	}
	me_state.triggers_topic_list_index++;


    while (1) {
        if(inverse){
            raw_val = 4096-adc1_get_raw(ADC_chan);
        }else{
			raw_val = adc1_get_raw(ADC_chan);
        }
        resault =resault*(1-k)+raw_val*k;


        if(abs(resault - prev_resault)>dead_band){
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

			if(flag_custom_topic){

				if(flag_float_output){
                    //sprintf(str,"%s:%f", custom_topic, f_res);
					sprintf(tmpString,"%s:%f", custom_topic, f_res);
                }else{
                    //sprintf(str,"%s:%d", custom_topic, resault);
					sprintf(tmpString,"%s:%d", custom_topic, resault);
                }
			}else{

                if(flag_float_output){
				    //sprintf(str,"%s/analog_%d:%f", me_config.device_name, num_of_slot, f_res);
					sprintf(tmpString,"%s/analog_%d:%f", me_config.device_name, num_of_slot, f_res);
                }else{
                    //sprintf(str,"%s/analog_%d:%d", me_config.device_name, num_of_slot, resault);
					sprintf(tmpString,"%s/analog_%d:%d", me_config.device_name, num_of_slot, resault);
                }
			}

			report(tmpString);
			//free(str); 

        }
        //ESP_LOGD(TAG, "analog val:%d", resault);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    
}

void start_analog_task(int num_of_slot){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = num_of_slot;
	// int num_of_slot = *(int*) arg;
	
	xTaskCreate(analog_task, "analog_task", 1024 * 4, &t_slot_num, 12, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "analog_task init ok: %d Heap usage: %lu free heap:%u", num_of_slot, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}