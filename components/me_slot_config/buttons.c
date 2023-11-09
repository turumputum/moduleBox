#include "buttons.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"


#include "reporter.h"
#include "stateConfig.h"

#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[6][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "BUTTONS";

void button_task(void *arg){
#define THRESHOLD 2
	int num_of_slot = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[num_of_slot][0];
	gpio_reset_pin(pin_num);
	esp_rom_gpio_pad_select_gpio(pin_num);
	gpio_set_pull_mode(pin_num, GPIO_PULLDOWN_ONLY);
	gpio_set_direction(pin_num, GPIO_MODE_INPUT);
	ESP_LOGD(TAG,"SETUP button_pin_%d Slot:%d", pin_num, num_of_slot );
	
	int button_state=0;
	int prev_state=0;
	char str[255];

	int button_inverse=0;
	if (strstr(me_config.slot_options[num_of_slot], "button_inverse")!=NULL){
		button_inverse=1;
	}

	uint8_t flag_custom_topic = 0;
    char* custom_topic=NULL;
    if (strstr(me_config.slot_options[num_of_slot], "custom_topic") != NULL) {
    	custom_topic = get_option_string_val(num_of_slot, "custom_topic");
		ESP_LOGD(TAG, "Custom topic:%s", custom_topic);
        flag_custom_topic = 1;
    }
    if (flag_custom_topic == 0) {
        char* str = calloc(strlen(me_config.device_name) + strlen("/button_") + 6, sizeof(char));
        sprintf(str, "%s/button_%d", me_config.device_name, num_of_slot);
        me_state.triggers_topic_list[me_state.triggers_topic_list_index] = str;
    }
    else {
        me_state.triggers_topic_list[me_state.triggers_topic_list_index] = custom_topic;
    }
    me_state.triggers_topic_list_index++;

	int  count=0;
	while(1){
		vTaskDelay(pdMS_TO_TICKS(10));
		if(gpio_get_level(pin_num)){
			button_state=button_inverse ? 0 : 1;
		}else{
			button_state=button_inverse ? 1 : 0;
		}
		if(button_state != prev_state){
			prev_state = button_state;

			memset(str, 0, strlen(str));
			//ESP_LOGD(TAG,"button_%d:%d inverse:%d", num_of_slot, button_state, button_inverse );
			if (flag_custom_topic) {
                sprintf(str, "%s:%d", custom_topic, button_state);
				//ESP_LOGD(TAG,"custom_topic:%s String:%s",custom_topic, str);
            }else {
                sprintf(str, "%s/button_%d:%d", me_config.device_name, num_of_slot, button_state);
            }
            report(str);
			ESP_LOGD(TAG,"String:%s", str);

		}
	}
}

void start_button_task(int num_of_slot){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = num_of_slot;
	char tmpString[60];
	sprintf(tmpString, "task_button_%d", num_of_slot);
	xTaskCreate(button_task, tmpString, 1024*4, &t_slot_num,12, NULL);

	ESP_LOGD(TAG,"Button task created for slot: %d Heap usage: %lu free heap:%u", num_of_slot, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


