#include <stdio.h>
#include "ticketDispenser.h"

#include "sdkconfig.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_timer.h"
#include "math.h"

#include "executor.h"
#include "esp_log.h"
#include "me_slot_config.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"


extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "TICKETDISPENSER";


static void IRAM_ATTR isr_handler(void* arg){
    int slot_num = (int) arg;
	uint8_t tmp=1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

void ticketDispenser_task(void *arg) {
	int slot_num = *(int*) arg;
	uint32_t heapBefore = xPortGetFreeHeapSize();
	//---init hardware---
	uint8_t outPin_num = SLOTS_PIN_MAP[slot_num][1];
	esp_rom_gpio_pad_select_gpio(outPin_num);
	gpio_set_direction(outPin_num, GPIO_MODE_OUTPUT);


    int inInverse = 0;
	if (strstr(me_config.slot_options[slot_num], "inInverse") != NULL) {
		inInverse = 1;
		ESP_LOGD(TAG, "Set inInverse:%d for slot:%d",inInverse, slot_num);
	}  

    uint8_t inPin_num = SLOTS_PIN_MAP[slot_num][0];
	me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));
	gpio_reset_pin(inPin_num);
	esp_rom_gpio_pad_select_gpio(inPin_num);
    gpio_config_t in_conf = {};
    if(inInverse){
   	    in_conf.intr_type = GPIO_INTR_POSEDGE;
    }else{
        in_conf.intr_type = GPIO_INTR_NEGEDGE;
    }
    //bit mask of the pins, use GPIO4/5 here
    in_conf.pin_bit_mask = (1ULL<<inPin_num);
    //set as input mode
    in_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&in_conf);
	gpio_set_intr_type(inPin_num, in_conf.intr_type);
    gpio_install_isr_service(0);
	gpio_isr_handler_add(inPin_num, isr_handler, (void*)slot_num);

    int debounce_gap = 100;
	if (strstr(me_config.slot_options[slot_num], "inDebounceGap") != NULL) {
		debounce_gap = get_option_int_val(slot_num, "inDebounceGap");
		ESP_LOGD(TAG, "Set debounce_gap:%d for slot:%d",debounce_gap, slot_num);
	}

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    //---set inverse---
	int out_inverse = 0;
	if (strstr(me_config.slot_options[slot_num], "outInverse") != NULL) {
		out_inverse = 1;
		ESP_LOGD(TAG, "Set out_inverse:%d for slot:%d",out_inverse, slot_num);
	}

    uint16_t outTimeout = 5*1000;
	if (strstr(me_config.slot_options[slot_num], "outTimeout") != NULL) {
		outTimeout = get_option_int_val(slot_num, "outTimeout")*1000;
		ESP_LOGD(TAG, "Set outTimeout:%d mSek for slot:%d",outTimeout, slot_num);
	}

    uint16_t overPrint = 200;
	if (strstr(me_config.slot_options[slot_num], "overPrint") != NULL) {
		overPrint = get_option_int_val(slot_num, "overPrint");
		ESP_LOGD(TAG, "Set overPrint:%d mSek for slot:%d",outTimeout, slot_num);
	}

    uint16_t blindGap = 200;
    if (strstr(me_config.slot_options[slot_num], "blindGap") != NULL) {
		blindGap = get_option_int_val(slot_num, "blindGap");
		ESP_LOGD(TAG, "Set blindGap:%d mSek for slot:%d",outTimeout, slot_num);
	}

    //---add action to topic list---
	if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/ticketDispenser_0")+3];
		sprintf(t_str, "%s/ticketDispenser_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	}

    uint16_t count=0;
    uint16_t targetCount = 99;

    uint8_t watchDogFlag=0;
    uint16_t watchdogCount = 0;

    uint32_t tick=xTaskGetTickCount();

    uint16_t timeKvant=30;

    waitForWorkPermit(slot_num);

    while(1){
        command_message_t msg;
		if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            targetCount = atoi(msg.str+strlen(me_state.action_topic_list[slot_num])+1);
            if (targetCount > 0) {
                ESP_LOGD(TAG, "ticketDispenser_task cmd: %s", msg.str);
                gpio_set_level(outPin_num, !out_inverse);
                vTaskDelay(pdMS_TO_TICKS(blindGap));
                watchDogFlag=1;
                count=0;
            }
        }

        uint8_t tmp;
		if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, 0) == pdPASS){
            if(debounce_gap!=0){
				if((xTaskGetTickCount()-tick)<debounce_gap){
					//ESP_LOGD(TAG, "Debounce skip delta:%ld",(xTaskGetTickCount()-tick));
					goto exit;
				}
			}
            count++;
            watchdogCount=0;
            tick = xTaskGetTickCount();
            ESP_LOGD(TAG, "ticketDispenser_task interrupt count:%d", count);
            exit:
        }

        if(count>=targetCount){
            vTaskDelay(pdMS_TO_TICKS(overPrint));
            gpio_set_level(outPin_num, out_inverse);
            count=0;
            watchDogFlag=0;
            ESP_LOGD(TAG, "issued");
        }

        if(watchDogFlag==1){
            watchdogCount+=timeKvant;
            if(watchdogCount>outTimeout){
                gpio_set_level(outPin_num, out_inverse);
                watchDogFlag=0;
                watchdogCount=0;
                char str[255];
                memset(str, 0, strlen(str));
				sprintf(str, "/error:noTicket");
                report(str, slot_num);
                ESP_LOGD(TAG, "watchdog outTimeout");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(timeKvant));
    }

}

void start_ticketDispenser_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_ticketDispenser_%d", slot_num);
	xTaskCreatePinnedToCore(ticketDispenser_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES-12, NULL, 1);

	ESP_LOGD(TAG,"ticketDispenser_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}