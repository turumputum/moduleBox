#include "flywheel.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#include "driver/ledc.h"

#include "reporter.h"
#include "executor.h"
#include "stateConfig.h"

#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "FLYWHEEL";

static void IRAM_ATTR gpio_isr_handler(void* arg){
    int slot_num = (int) arg;
	uint8_t tmp=1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

void flywheel_task(void *arg){
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][0];

	me_state.interrupt_queue[slot_num] = xQueueCreate(5, sizeof(uint8_t));

	gpio_reset_pin(pin_num);
	esp_rom_gpio_pad_select_gpio(pin_num);
    gpio_config_t in_conf = {};
   	in_conf.intr_type = GPIO_INTR_ANYEDGE;
    in_conf.pin_bit_mask = (1ULL<<pin_num);
    in_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&in_conf);
	gpio_set_intr_type(pin_num, GPIO_INTR_ANYEDGE);
    gpio_install_isr_service(0);
	gpio_isr_handler_add(pin_num, gpio_isr_handler, (void*)slot_num);
	
	ESP_LOGD(TAG,"SETUP sensor_pin_%d Slot:%d", pin_num, slot_num );
	
	uint8_t sensor_state=0;
	int prev_state=0;
	char str[255];

	int sensor_inverse=0;
	if (strstr(me_config.slot_options[slot_num], "sensor_inverse")!=NULL){
		sensor_inverse=1;
	}

    int debug_report=0;
    if (strstr(me_config.slot_options[slot_num], "debug_report")!=NULL){
		debug_report=1;
	}

    float decrement=0.5;
    if (strstr(me_config.slot_options[slot_num], "decrement") != NULL) {
		decrement = get_option_float_val(slot_num, "decrement");
		ESP_LOGD(TAG, "Set decrement:%f for slot:%d",decrement, slot_num);
	}

	int max_counter = 20;
	if (strstr(me_config.slot_options[slot_num], "max_counter") != NULL) {
		max_counter = get_option_int_val(slot_num, "max_counter");
		ESP_LOGD(TAG, "Set max_counter:%d for slot:%d", max_counter, slot_num);
	}

	int debounce_delay = 0;
	if (strstr(me_config.slot_options[slot_num], "sensor_debounce_delay") != NULL) {
		debounce_delay = get_option_int_val(slot_num, "sensor_debounce_delay");
		ESP_LOGD(TAG, "Set debounce_delay:%d for slot:%d",debounce_delay, slot_num);
	}
    
    if (strstr(me_config.slot_options[slot_num], "flywheel_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "flywheel_topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/flywheel_0")+3];
		sprintf(t_str, "%s/flywheel_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}

	uint32_t debounce_tick=xTaskGetTickCount();
    uint32_t decrement_tick=xTaskGetTickCount();

    float flywheel_counter=0;
    uint8_t flywheel_state=0;
    uint8_t _flywheel_state=0;
    for(;;) {
		uint8_t tmp;
        if((xTaskGetTickCount()-decrement_tick)>1000){
            decrement_tick=xTaskGetTickCount();
            
            if(flywheel_counter>0.1){
                flywheel_counter-=decrement;
                if(debug_report){
                    ESP_LOGD(TAG, "flywheel_counter:%f", flywheel_counter);
                    char str[255];
                    memset(str, 0, strlen(str));
                    sprintf(str, "/counter:%f", flywheel_counter);
                    report(str, slot_num);
                }
                if(flywheel_counter<0.1){
                    flywheel_counter=0;
                }
            }
            
            if(flywheel_counter>0.1){
                flywheel_state=1;
            }else{
                flywheel_state=0;
            }

            if(flywheel_state!= _flywheel_state){
                _flywheel_state=flywheel_state;
                ESP_LOGD(TAG, "Flywheel_state:%d", flywheel_state);
                char str[255];
                memset(str, 0, strlen(str));
                sprintf(str, "%d", flywheel_state);
                report(str, slot_num);
            }

        }

		if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, 100) == pdPASS){
			//ESP_LOGD(TAG,"%ld :: Incoming int_msg:%d",xTaskGetTickCount(), tmp);

			if(gpio_get_level(pin_num)){
				sensor_state=sensor_inverse ? 0 : 1;
			}else{
				sensor_state=sensor_inverse ? 1 : 0;
			}

			if(debounce_delay!=0){
				if((xTaskGetTickCount()-debounce_tick)<debounce_delay){
					ESP_LOGD(TAG, "Debounce skip delta:%ld",(xTaskGetTickCount()-debounce_tick));
					goto exit;
				}
			}
			
			if(sensor_state != prev_state){
				prev_state = sensor_state;
				debounce_tick = xTaskGetTickCount();
                if(sensor_state==1){
                    flywheel_counter+=1;
					if(flywheel_counter>max_counter){
						flywheel_counter=max_counter;	
					}
                    if(debug_report){
                        ESP_LOGD(TAG, "flywheel_counter:%f", flywheel_counter);
                        char str[255];
                        memset(str, 0, strlen(str));
                        sprintf(str, "/counter:%f", flywheel_counter);
                        report(str, slot_num);
                    }
                }
			}

			exit:
		}
    }

}

void start_flywheel_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "flywheel_task_%d", slot_num);
	xTaskCreate(flywheel_task, tmpString, 1024*4, &t_slot_num,12, NULL);

	ESP_LOGD(TAG,"flywheel_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}
