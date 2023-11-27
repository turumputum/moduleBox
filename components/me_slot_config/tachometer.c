#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/timer.h"
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
static const char *TAG = "TACHOMETER";

typedef struct
{
	uint8_t pulse_flag;
	uint64_t last_tick;
	uint16_t rpm;
    uint16_t prev_rpm;
} tachoWork_var;

uint64_t debug_flag;

static void IRAM_ATTR up_front_handler(void *args)
{
	
	tachoWork_var *var = (tachoWork_var *)args;
	var->pulse_flag=1;
	
	
	
	//var->last_tick = esp_timer_get_time();
	//debug_flag=delta;
	//timer_set_counter_value();
}

void tachometer_task(void *arg)
{
	uint16_t resault=0, prev_resault=0;
	uint8_t flag_report=0;


	int slot_num = *(int *)arg;
	uint8_t sens_pin_num = SLOTS_PIN_MAP[slot_num][0];

    ESP_ERROR_CHECK(gpio_reset_pin(sens_pin_num)); 
    ESP_ERROR_CHECK(gpio_set_direction(sens_pin_num, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_intr_type(sens_pin_num, GPIO_INTR_POSEDGE));
    gpio_install_isr_service(0);
    tachoWork_var var;
    ESP_ERROR_CHECK(gpio_isr_handler_add(sens_pin_num, up_front_handler, (void *)&var));

    uint16_t threshold  = 0;
	if (strstr(me_config.slot_options[slot_num], "threshold")!=NULL){
		threshold = get_option_int_val(slot_num, "threshold");
		if (threshold <= 0)
		{
			ESP_LOGD(TAG, "threshold wrong format, set default. Slot:%d", slot_num);
			threshold = 0; // default val
		}
	}

	uint8_t inverse  = 0;
	if (strstr(me_config.slot_options[slot_num], "inverse")!=NULL){
		inverse=1;
	}

	uint8_t flag_custom_topic = 0;
	char *custom_topic;
	if (strstr(me_config.slot_options[slot_num], "custom_topic")!=NULL){
		custom_topic = get_option_string_val(slot_num,"custom_topic");
		//get_option_string_val(slot_num,"custom_topic", &custom_topic);
		ESP_LOGD(TAG, "Custom topic:%s", custom_topic);
		flag_custom_topic=1;
	}

	
	if(flag_custom_topic==0){
		char *str = calloc(strlen(me_config.device_name)+strlen("/tachometer_")+4, sizeof(char));
		sprintf(str, "%s/tachometer_%d",me_config.device_name, slot_num);
		me_state.trigger_topic_list[slot_num]=str;
	}else{
		me_state.trigger_topic_list[slot_num]=custom_topic;
	}
			
	while (1)
	{
		if((esp_timer_get_time() - var.last_tick)>1000000){
			var.pulse_flag=1;
		}

		if(var.pulse_flag){
			var.pulse_flag=0;

			uint64_t delta = esp_timer_get_time()-var.last_tick;
			if((delta>0)&&(delta<1000000)){
				var.rpm = (int)((float)60000000/delta);
			}else{
				var.rpm = 0;
			}
			var.last_tick = esp_timer_get_time();
			//ESP_LOGD(TAG,"DEBUG:%d", var.rpm);
		}


		if (var.prev_rpm != var.rpm){	
			if(threshold==0){
				resault = var.rpm;
				flag_report=1;
			}else{
				if(var.rpm>=threshold){
					resault=!inverse;
				}else{
					resault = inverse;
				}
				if(resault!=prev_resault){
					flag_report=1;
					prev_resault = resault;
				}
			}
			var.prev_rpm=var.rpm;
		}

		if(flag_report){
			flag_report=0;
			int str_len;//=strlen(me_config.device_name)+strlen("/tachometer_")+8;
			char *str;// = (char*)malloc(str_len * sizeof(char));

			if(flag_custom_topic){
				str_len=strlen(custom_topic)+4;
				str = (char*)malloc(str_len * sizeof(char));
				sprintf(str,"%s:%d", custom_topic, resault);
			}else{
				str_len=strlen(me_config.device_name)+strlen("/tachometer_")+8;
				str = (char*)malloc(str_len * sizeof(char));
				sprintf(str,"%s/tachometer_%d:%d", me_config.device_name, slot_num, resault);
			}

			report(str, 0);
			free(str); 
		}
			
			
		


		if(debug_flag){
			debug_flag=0;
			ESP_LOGD(TAG,"DEBUG:%llu", debug_flag);
		}
		uint64_t tmp;
		//timer_get_counter_value(TIMER_GROUP_0, TIMER_0, &tmp);
		//ESP_LOGD(TAG,"DEBUG:%llu", tmp);
        vTaskDelay(pdMS_TO_TICKS(2));
	}
}

void start_tachometer_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	// int slot_num = *(int*) arg;
	
	xTaskCreate(tachometer_task, "tachometer_task", 1024 * 4, &t_slot_num, 12, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "tachometer_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}