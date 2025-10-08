#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "me_slot_config.h"

#include "executor.h"
#include "stateConfig.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "3n_mosfet.h"
#include "stepper.h"
#include "in_out.h"
#include "reporter.h"
#include "myCDC.h"
#include "audioPlayer.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

exec_message_t exec_message;
QueueHandle_t exec_mailbox;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "EXECUTOR";


void init_led(int slot_num) {
	uint32_t heapBefore = xPortGetFreeHeapSize();
	//---init hardware---
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
	//printf("slot:%d pin:%d \r\n", slot_num, pin_num);
	esp_rom_gpio_pad_select_gpio(pin_num);
	gpio_set_direction(pin_num, GPIO_MODE_OUTPUT);

	//---set default state---
	int optorelay_inverse = 0;
	if (strstr(me_config.slot_options[slot_num], "led_inverse") != NULL) {
		optorelay_inverse = 1;
	}

	uint8_t def_state = optorelay_inverse;
	if (strstr(me_config.slot_options[slot_num], "led_default_high") != NULL) {
		if (strstr(me_config.slot_options[slot_num], "led_inverse") != NULL) {
		} else {
			def_state = !def_state;
		}
	}
	ESP_ERROR_CHECK(gpio_set_level(pin_num, (uint32_t )def_state));

	//---add action to topic list---
	char *str = calloc(strlen(me_config.deviceName) + 16, sizeof(char));
	sprintf(str, "%s/led_%d", me_config.deviceName, slot_num);
	me_state.action_topic_list[slot_num] = str;

	ESP_LOGD(TAG, "Led inited for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

typedef struct {
	uint8_t pin;
	int interval;
} FlashArgs_t;

void led_off_delay_task(void *pvParameters) {
	
	uint8_t slot_num = *((uint8_t*) pvParameters);
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
	int off_delay;
	char *rest;
	//ESP_LOGD(TAG, "Vot huinya:");
	char *tok = strstr(me_config.slot_options[slot_num], "off_delay");
	if (strstr(me_config.slot_options[slot_num], ",") != NULL) {
		tok = strtok_r(tok, ",", &rest);
	}
	//ESP_LOGD(TAG, "---option is:%s", tok);
	if (strstr(tok, ":") != NULL) {
		tok = strstr(tok, ":") + 1;
		//ESP_LOGD(TAG, "---value is:%s", tok);
		off_delay = atoi(tok);
	}else{
		off_delay = 3;
	}
	ESP_LOGD(TAG, "Start off_delay slot:%d delay_sek:%d", slot_num, off_delay);
	vTaskDelay(pdMS_TO_TICKS(off_delay*1000));
	gpio_set_level(pin_num, 0);
	me_state.slot_task[slot_num]=NULL;
	vTaskDelete(NULL);
}

void flash_led_task(void *pvParameters) {

	uint8_t slot_num = *((uint8_t*) pvParameters);
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
	int flash_interval;
	char *rest;
	char *tok = strstr(me_config.slot_options[slot_num], "flash");
	if (strstr(me_config.slot_options[slot_num], ",") != NULL) {
		tok = strtok_r(tok, ",", &rest);
	}
	//ESP_LOGD(TAG, "---option is:%s", tok);
	if (strstr(tok, ":") != NULL) {
		tok = strstr(tok, ":") + 1;
		//ESP_LOGD(TAG, "---value is:%s", tok);
		flash_interval = atoi(tok);
	}else{
		flash_interval = 500;
	}

	ESP_LOGD(TAG, "Start flash task for slot:%d interval:%d", slot_num, flash_interval);

	int level = 0;
	while (1) {
		ESP_LOGD(TAG, "task work, level:%d", level);
		level = !level;

		gpio_set_level(pin_num, level);
		if(level==0){
			vTaskDelay(pdMS_TO_TICKS(flash_interval*2));
		}else{
			vTaskDelay(pdMS_TO_TICKS(flash_interval));
		}
	}
}

void exec_led(int slot_num, int payload) {
	int led_inverse = 0;

	if (strstr(me_config.slot_options[slot_num], "led_inverse") != NULL) {
		led_inverse = 1;
	}

	ESP_LOGD(TAG, "Led option string:%s", me_config.slot_options[slot_num]);
	if (strstr(me_config.slot_options[slot_num], "flash") != NULL) {
		if (payload == 1) {
			if (me_state.slot_task[slot_num] == NULL) {
				//ESP_LOGD(TAG, "Led start flash task with interval:%d", flash_interval);
				xTaskCreate(flash_led_task, "", 1024 * 3, &slot_num, 6, &me_state.slot_task[slot_num]);
			}else{
				eTaskState taskState = eTaskGetState(me_state.slot_task[slot_num]);
				//ESP_LOGD(TAG, "---beforeStart taskState:%d", taskState);
				if(taskState==eDeleted){
					xTaskCreate(flash_led_task, "", 1024 * 3, &slot_num, 6, &me_state.slot_task[slot_num]);
				}else{
					ESP_LOGD(TAG, "Task is running");
				}
			}
		} else if (payload == 0) {
			if (me_state.slot_task[slot_num] != NULL) {
				eTaskState taskState = eTaskGetState(me_state.slot_task[slot_num]);
				ESP_LOGD(TAG, "---beforeStop taskState:%d", taskState);
				if(taskState!=eDeleted){
					uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
					vTaskDelete(me_state.slot_task[slot_num]);
					gpio_set_level(pin_num, 0);
					ESP_LOGD(TAG, "Led flash task STOP");
				}else{
					ESP_LOGD(TAG, "Task is stopped");
				}
			}
		}
	}else if (strstr(me_config.slot_options[slot_num], "off_delay") != NULL) {
		uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
		if (payload == 1) {
			gpio_set_level(pin_num, 1);
			if (me_state.slot_task[slot_num] != NULL) {
				eTaskState taskState = eTaskGetState(me_state.slot_task[slot_num]);
				if(taskState!=eDeleted){
					ESP_LOGD(TAG, "Delete TASK for slot:%d taskState:%d", slot_num, taskState);
					vTaskDelete(me_state.slot_task[slot_num]);
				}
			}
			xTaskCreate(led_off_delay_task, "", 1024 * 3, &slot_num, 6, &me_state.slot_task[slot_num]);
			ESP_LOGD(TAG, "Off delay task started for slot:%d", slot_num);
			
		}else{
			gpio_set_level(pin_num, 0);
		}
	}else {
		uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
		int level = led_inverse ? !payload : payload;
		gpio_set_level(pin_num, level);
		//printf("pin_num:%d\n", pin_num);
		ESP_LOGD(TAG, "Led set:%d for slot:%d inverse:%d level:%d", payload, slot_num, led_inverse, level);
	}


}

void execute(char *action) {
	ESP_LOGD(TAG, "Execute action:%s", action);
	exec_message_t msg;
	strcpy(msg.str, action);

	//ESP_LOGE(TAG, "execute >>");
	if(xQueueSend(me_state.executor_queue, &msg, portMAX_DELAY)!= pdPASS) {
		ESP_LOGE(TAG, "Send message FAIL");
	}

	//ESP_LOGE(TAG, "execute <<");
}

void executer_task(void * param){
	exec_message_t msg;
	me_state.executor_queue = xQueueCreate(150, sizeof(exec_message_t));

	vTaskDelay(pdMS_TO_TICKS(2000));

	while(1){
		if (xQueueReceive(me_state.executor_queue, &msg, portMAX_DELAY) == pdPASS){
			//ESP_LOGD(TAG, "incoming cmd:%s", msg.str);
			int sum=0;
			if(strstr(msg.str, "getState")!=NULL){
				ESP_LOGD(TAG, "Get state");
				reportState();
				sum++;
			}else if(strstr(msg.str, "system/restart")!=NULL){
				ESP_LOGD(TAG, "restart");
				esp_restart();
				sum++;

			}else if(strstr(msg.str, "status")!=NULL){				
				makeStatusReport(true);
				sum++;
			}else{
				for(int i=0; i<NUM_OF_SLOTS; i++){
					//ESP_LOGD(TAG, "command_queue[%d]:%d",i,me_state.command_queue[i]==NULL);
					if(strstr(msg.str, me_state.action_topic_list[i])!=NULL){
						//ESP_LOGD(TAG, "Forward cmd:%s to slot:%d", msg.str, i);
						if(me_state.command_queue[i]!=NULL){
							xQueueSend(me_state.command_queue[i], &msg, portMAX_DELAY);
							sum++;
						}else{
							ESP_LOGE(TAG, "Slot queue is not initialized");
						}
					}
				}
			}
			if(sum==0){
				usbprint("Action not found!!!");
				ESP_LOGE(TAG, "Action not found: %s", msg.str);
			}
		}
	}
}
