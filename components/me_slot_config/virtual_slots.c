#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "virtual_slots.h"

#include "executor.h"
#include "reporter.h"
#include "stateConfig.h"

#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "VIRTUAL_SLOTS";

void startup_task(void *arg) {
    int slot_num = *(int*) arg;

    int delay = 50; // 
    if (strstr(me_config.slot_options[slot_num], "delay") != NULL) {
		delay = get_option_int_val(slot_num, "delay");
		ESP_LOGD(TAG, "Set delay :%d for slot:%d",delay, slot_num);
	}


    if (strstr(me_config.slot_options[slot_num], "startup_report_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "startup_report_topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "startup_report_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.device_name)+strlen("/startup")+3];
		sprintf(t_str, "%s/startup",me_config.device_name);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart startup_topic:%s", me_state.trigger_topic_list[slot_num]);
	}

    if(delay!=0){
        vTaskDelay(delay);
    }
    char tmpString[3];
    sprintf(tmpString, "1");
    report(tmpString, slot_num);

    
    ESP_LOGD(TAG, "Startup task delete");
    vTaskDelay(50);
    vTaskDelete(NULL);
}

void start_startup_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "startup_task_%d", slot_num);
	xTaskCreatePinnedToCore(startup_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "startup_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

void counter_task(void *arg) {
    int slot_num = *(int*) arg;

    int counter = 0; //

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    int minVal = 0; // 
    if (strstr(me_config.slot_options[slot_num], "minVal") != NULL) {
		minVal = get_option_int_val(slot_num, "minVal");
		ESP_LOGD(TAG, "Set minVal :%d for slot:%d", minVal, slot_num);
	}

    int maxVal = 32766; // 
    if (strstr(me_config.slot_options[slot_num], "maxVal") != NULL) {
		maxVal = get_option_int_val(slot_num, "maxVal");
		ESP_LOGD(TAG, "Set maxVal :%d for slot:%d", maxVal, slot_num);
	}

    int threshold = -1; // 
    if (strstr(me_config.slot_options[slot_num], "threshold") != NULL) {
		threshold = get_option_int_val(slot_num, "threshold");
		ESP_LOGD(TAG, "Set threshold :%d for slot:%d", threshold, slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "counter_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "counter_topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.device_name)+strlen("/counter_0")+3];
		sprintf(t_str, "%s/counter_%d",me_config.device_name, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
        me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);
	}

    int state=0;
    int prevState=0;

    while(1){
		command_message_t cmd;
		if (xQueueReceive(me_state.command_queue[slot_num], &cmd, portMAX_DELAY) == pdPASS){
			char *command=cmd.str+strlen(me_state.action_topic_list[slot_num])+1;
			if(strstr(command, ":")==NULL){
                break;
            }
            char *cmd_arg = strstr(command, ":")+1;
			ESP_LOGD(TAG, "Incoming command:%s  arg:%s", command, cmd_arg); 
			if(!memcmp(command, "set", 3)){//------------------------------
				if(cmd_arg[0]=='+'){
					counter+= atoi(cmd_arg+1);
				}else if(cmd_arg[0]=='-'){
					counter-= atoi(cmd_arg+1);
				}else{
					counter=atoi(cmd_arg);
				}

                if(counter<minVal){
                    counter=minVal;
                }else if(counter>maxVal){
                    counter=maxVal;
                }

                if(threshold>0){
                    if(counter>=threshold){
                        state=1;
                    }else {
                        state=0;
                    }
                }else{
                    state=counter;
                }

                if(state!=prevState){
                    prevState=state;

                    char tmpString[60];
                    sprintf(tmpString, "%d", state);
                    report(tmpString, slot_num);
                }
          
			}
        }
    }

}

void start_counter_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "counter_task_%d", slot_num);
	xTaskCreatePinnedToCore(counter_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "counter_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

//---------------------------------TIMER---------------------------------
static void IRAM_ATTR timer_isr_handler(void* arg){
    int slot_num = (int) arg;
	uint8_t tmp=1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

void timer_task(void *arg) {
    int slot_num = *(int*) arg;

    int counter = 0; //

    me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));
    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    uint16_t time = 0; // 
    if (strstr(me_config.slot_options[slot_num], "time") != NULL) {
		time = get_option_int_val(slot_num, "time");
		ESP_LOGD(TAG, "Set time :%d for slot:%d", time, slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "timer_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "timer_topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.device_name)+strlen("/timer_0")+3];
		sprintf(t_str, "%s/timer_%d",me_config.device_name, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
        me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);
	}

    int state=0;
    esp_timer_handle_t virtual_timer;
    

    while(1){
		vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t tmp;
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, 0) == pdPASS){
			report("/timerEnd:1", slot_num);
            //ESP_LOGD(TAG,"%ld :: Incoming int_msg:%d",xTaskGetTickCount(), tmp);
        }
        command_message_t cmd;
		if (xQueueReceive(me_state.command_queue[slot_num], &cmd, 0) == pdPASS){
			char *command=cmd.str+strlen(me_state.action_topic_list[slot_num])+1;
			char *cmd_arg = NULL;
			if(strstr(command, ":")!=NULL){
				cmd_arg = strstr(command, ":")+1;
			}else{
				cmd_arg = strdup("0");
			}
			//ESP_LOGD(TAG, "Incoming command:%s  arg:%s", command, cmd_arg); 
			if(!memcmp(command, "start", 5)){ 
                //char *payload = strdup(cmd.str+strlen(me_state.action_topic_list[slot_num])+strlen()+1);
                //ESP_LOGD(TAG, "slot_num:%d payload:%s",slot_num, payload);
                int val = atoi(cmd_arg);
                //ESP_LOGD(TAG, "slot_num:%d val:%d",slot_num, val);
                if(val==0){
                    val=time;
                }
                
                const esp_timer_create_args_t delay_timer_args = {
                    .callback = &timer_isr_handler,
                    .arg = (void*)slot_num,
                    .name = "virtual_timer"
                };
                esp_timer_create(&delay_timer_args, &virtual_timer);
                esp_timer_start_once(virtual_timer, val*1000);
			}else if(!memcmp(command, "stop", 4)){
                esp_err_t ret = esp_timer_stop(virtual_timer);
                if(ret!=ESP_OK){
                    ESP_LOGD(TAG, "stop timer error:%s", esp_err_to_name(ret));
                }
                ret = esp_timer_delete(virtual_timer);
                if(ret!=ESP_OK){
                    ESP_LOGD(TAG, "delete timer error:%s", esp_err_to_name(ret));
                }
            }
        }
    }

}

void start_timer_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "timer_task_%d", slot_num);
	xTaskCreatePinnedToCore(timer_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "timer_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


//---------------------------------WATCHDOG---------------------------------
static void IRAM_ATTR watchdog_isr_handler(void* arg){
    int slot_num = (int) arg;
	uint8_t tmp=1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

void watchdog_task(void *arg) {
    int slot_num = *(int*) arg;

    me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));
    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    uint32_t time = 3600; // in sek
    if (strstr(me_config.slot_options[slot_num], "time") != NULL) {
		time = get_option_int_val(slot_num, "time");
	}
    ESP_LOGD(TAG, "Set time :%ld for slot:%d", time, slot_num);

    char t_str[strlen(me_config.device_name)+strlen("/watchdog_0")+3];
    sprintf(t_str, "%s/watchdog_%d",me_config.device_name, slot_num);
    me_state.trigger_topic_list[slot_num]=strdup(t_str);
    me_state.action_topic_list[slot_num]=strdup(t_str);
    ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);

    int strl = strlen(me_state.action_topic_list[slot_num])+strlen("restart")+1;
    char tmpstr[strl];
    sprintf(tmpstr, "%s/%s", me_state.action_topic_list[slot_num], "restart");
    xQueueSend(me_state.command_queue[slot_num], &tmpstr, NULL);
    
    esp_timer_handle_t virtual_timer=NULL;
    
    while(1){
		vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t tmp;
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, 0) == pdPASS){
			//report("/timerEnd:1", slot_num);
            ESP_LOGD(TAG,"%ld :: watchdog reset",xTaskGetTickCount());
            vTaskDelay(1000);
            esp_restart();
        }
        command_message_t cmd;
		if (xQueueReceive(me_state.command_queue[slot_num], &cmd, 0) == pdPASS){
			char *command=cmd.str+strlen(me_state.action_topic_list[slot_num])+1;
			char *cmd_arg = NULL;
			if(strstr(command, ":")!=NULL){
				cmd_arg = strstr(command, ":")+1;
			}else{
				cmd_arg = strdup("0");
			}
			//ESP_LOGD(TAG, "Incoming command:%s  arg:%s", command, cmd_arg); 
			if(!memcmp(command, "restart", 7)){ 
                
                if(virtual_timer!=NULL){
                    esp_timer_stop(virtual_timer);
                }

                const esp_timer_create_args_t delay_timer_args = {
                    .callback = &watchdog_isr_handler,
                    .arg = (void*)slot_num,
                    .name = "watchdog_timer"
                };
                esp_timer_create(&delay_timer_args, &virtual_timer);
                esp_timer_start_once(virtual_timer, time*1000*1000);  
                ESP_LOGD(TAG, "Start watchdog on time:%ld",time);
            }
        }
    }

}

void start_watchdog_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "watchdog_task_%d", slot_num);
	xTaskCreatePinnedToCore(watchdog_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "watchdog_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}