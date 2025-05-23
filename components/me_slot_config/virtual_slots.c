#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
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

//-----------------------------------------startUp--------------------------------
void startup_task(void *arg) {
    int slot_num = *(int*) arg;

    int delay = 50; // 
    if (strstr(me_config.slot_options[slot_num], "delay") != NULL) {
		delay = get_option_int_val(slot_num, "delay");
		ESP_LOGD(TAG, "Set delay :%d for slot:%d",delay, slot_num);
	}


    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "startup_report_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/startup")+3];
		sprintf(t_str, "%s/startup",me_config.deviceName);
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

//---------------------------------COUNTER---------------------------------
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

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/counter_0")+3];
		sprintf(t_str, "%s/counter_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
        me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);
	}

    int state=0;
    int prevState=0;

    waitForWorkPermit(slot_num);

    while(1){
		command_message_t cmd;
		if (xQueueReceive(me_state.command_queue[slot_num], &cmd, portMAX_DELAY) == pdPASS){
			char *command=cmd.str+strlen(me_state.action_topic_list[slot_num])+1;
			if(strstr(command, ":")==NULL){
                ESP_LOGD(TAG, "No arguments found. EXIT"); 
            }else{
                char *cmd_arg = strstr(command, ":")+1;
                //ESP_LOGD(TAG, "Incoming command:%s  arg:%s", command, cmd_arg); 
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

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/timer_0")+3];
		sprintf(t_str, "%s/timer_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
        me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);
	}

    int state=0;
    esp_timer_handle_t virtual_timer;
    const esp_timer_create_args_t delay_timer_args = {
        .callback = &timer_isr_handler,
        .arg = (void*)slot_num,
        .name = "virtual_timer"
    };
    esp_timer_create(&delay_timer_args, &virtual_timer);

    waitForWorkPermit(slot_num);

    while(1){
		//vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t tmp;
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, 10) == pdPASS){
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

                if(esp_timer_is_active(virtual_timer)){
                    //ESP_LOGD(TAG, "timer already started, lets stop it");
                    esp_timer_stop(virtual_timer);
                }

                esp_timer_start_once(virtual_timer, (val-20)*1000);///20ms report delay
			}else if(!memcmp(command, "stop", 4)){
                //ESP_LOGD(TAG, "stop timeer slot:%d", slot_num);
                esp_err_t ret = esp_timer_stop(virtual_timer);
                if(ret!=ESP_OK){
                    ESP_LOGE(TAG, "stop timer error:%s", esp_err_to_name(ret));
                }
                // ret = esp_timer_delete(virtual_timer);
                // if(ret!=ESP_OK){
                //     ESP_LOGE(TAG, "delete timer error:%s", esp_err_to_name(ret));
                // }
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

//---------------------------------FLYWHEEL--------------------------------
void flywheel_task(void *arg){
	int slot_num = *(int*) arg;

	me_state.command_queue[slot_num] = xQueueCreate(25, sizeof(command_message_t));

    float decrement=0.1;
    if (strstr(me_config.slot_options[slot_num], "decrement") != NULL) {
		decrement = get_option_float_val(slot_num, "decrement");
		ESP_LOGD(TAG, "Set decrement:%f for slot:%d",decrement, slot_num);
	}

    uint16_t period = 100;
    if (strstr(me_config.slot_options[slot_num], "period") != NULL) {
		period = get_option_int_val(slot_num, "period");
		ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",period, slot_num);
	}

    uint16_t threshold = 0;
    if (strstr(me_config.slot_options[slot_num], "threshold") != NULL) {
        threshold = get_option_int_val(slot_num, "threshold");
        if (threshold <= 0){
            ESP_LOGE(TAG, "threshold wrong format, set default. Slot:%d", slot_num);
            threshold = 0; // default val
        }else{
            ESP_LOGD(TAG, "threshold:%d. Slot:%d", threshold, slot_num);
        }
    }

	int32_t maxVal = 20;
	if (strstr(me_config.slot_options[slot_num], "maxVal") != NULL) {
		maxVal = get_option_int_val(slot_num, "maxVal");
		ESP_LOGD(TAG, "Set max_counter:%ld for slot:%d", maxVal, slot_num);
	}

    int32_t minVal = 0;
	if (strstr(me_config.slot_options[slot_num], "minVal") != NULL) {
		minVal = get_option_int_val(slot_num, "minVal");
		ESP_LOGD(TAG, "Set min_counter:%ld for slot:%d", minVal, slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/flywheel_0")+3];
		sprintf(t_str, "%s/flywheel_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
        me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}

    float flywheelCount=0;
    float _flywheelCount=0;
    uint8_t flywheel_state=0;
    uint8_t _flywheel_state=0;

    TickType_t lastWakeTime = xTaskGetTickCount();

    waitForWorkPermit(slot_num);

    for(;;) {
        flywheelCount-=decrement;
        if(flywheelCount<minVal){
            flywheelCount=minVal;
        }

        if(threshold>0){
            if(flywheelCount>threshold){
                flywheel_state=1;
            }else{
                flywheel_state=0;
            }

            if(flywheel_state!= _flywheel_state){
                _flywheel_state=flywheel_state;
                //ESP_LOGD(TAG, "Flywheel_state:%d", flywheel_state);
                char str[3];
                memset(str, 0, strlen(str));
                sprintf(str, "%d", flywheel_state);
                report(str, slot_num);
            }
        }else{
            if((int)flywheelCount!=(int)_flywheelCount){
                _flywheelCount=flywheelCount;
                //ESP_LOGD(TAG, "Flywheel_state:%d", flywheel_state);
                char str[10];
                memset(str, 0, strlen(str));
                sprintf(str, "/count:%d", (int)flywheelCount);
                report(str, slot_num);
            }
        }

        //----------------------CHEKING COMMAND QUEUE----------------------------
        command_message_t msg;
        uint8_t recv_state=0;

        while(xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS) {
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            cmd = cmd + strlen(me_state.action_topic_list[slot_num])+1;
            if(strstr(cmd, "setCount")!=NULL){
                if(strstr(payload, "+")!=NULL){
                    flywheelCount+=atoi(payload);
                }else if(strstr(payload, "-")!=NULL){
                    flywheelCount-=atoi(payload);
                }else{
                    flywheelCount=atoi(payload);
                }
                if(flywheelCount>maxVal) flywheelCount=maxVal;
                if(flywheelCount<minVal) flywheelCount=minVal;
                ESP_LOGD(TAG, "Set flywheelCount:%f for slot:%d", flywheelCount, slot_num);
            }
        }
        vTaskDelayUntil(&lastWakeTime, period);
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

    char t_str[strlen(me_config.deviceName)+strlen("/watchdog_0")+3];
    sprintf(t_str, "%s/watchdog_%d",me_config.deviceName, slot_num);
    me_state.trigger_topic_list[slot_num]=strdup(t_str);
    me_state.action_topic_list[slot_num]=strdup(t_str);
    ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);

    int strl = strlen(me_state.action_topic_list[slot_num])+strlen("restart")+1;
    char tmpstr[strl];
    sprintf(tmpstr, "%s/%s", me_state.action_topic_list[slot_num], "restart");
    xQueueSend(me_state.command_queue[slot_num], &tmpstr, NULL);
    
    esp_timer_handle_t virtual_timer=NULL;
    const esp_timer_create_args_t delay_timer_args = {
        .callback = &watchdog_isr_handler,
        .arg = (void*)slot_num,
        .name = "watchdog_timer"
    };
    esp_timer_create(&delay_timer_args, &virtual_timer);

    waitForWorkPermit(slot_num);

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
                
                if(esp_timer_is_active(virtual_timer)){
                    esp_timer_stop(virtual_timer);
                }
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


//---------------------------------WHITELIST---------------------------------
void whitelist_task(void *arg) {
    #define MAX_LINE_LENGTH 256

    int slot_num = *(int*) arg;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    char filename[MAX_LINE_LENGTH]; // 
    strcpy(filename, "/sdcard/");
    if (strstr(me_config.slot_options[slot_num], "filename") != NULL) {
		strcat(filename,get_option_string_val(slot_num, "filename"));
	}else{
        strcat(filename,"whitelist.txt");
    }
    if (access(filename, F_OK) != 0) {
        char errorString[300];
        sprintf(errorString, "whitelist file: %s, does not exist", filename);
        ESP_LOGE(TAG, "%s", errorString);
        writeErrorTxt(errorString);
        vTaskDelay(200);
        vTaskDelete(NULL);
    }
    ESP_LOGD(TAG, "Set filename :%s for slot:%d", filename, slot_num);


    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/whitelist_0")+3];
		sprintf(t_str, "%s/whitelist_%d",me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num]=strdup(t_str);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
	}

    waitForWorkPermit(slot_num);
    
    while(1){
        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, portMAX_DELAY) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload = NULL;
            char* cmd = msg.str;
            uint16_t count=0;
            if(strstr(cmd, ":")!=NULL){
                cmd = strtok_r(msg.str, ":", &payload);
                //ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            }else{
                //ESP_LOGD(TAG, "Input command %s", cmd);
            }
            if(payload!=NULL){
                FILE* file = fopen(filename, "r");
                if (file == NULL) {
                    //ESP_LOGE(TAG, "Failed to open file");
                    goto end;
                }
                char line[MAX_LINE_LENGTH];
                while (fgets(line, sizeof(line), file)) {
                    // Удалить символ новой строки
                    line[strcspn(line, "\n")] = '\0';
                    //ESP_LOGD(TAG, "Read line:%s", line);
                    // Разделить строку на части до и после "->"                  
                    char* validValue=NULL;
                    char* command=NULL;
                    if(strstr(line, "->")!=NULL){
                        validValue = strtok_r(line, "->", &command);
                        command++;
                        //ESP_LOGD(TAG, "whitelist val:%s action:%s", validValue, command);
                    }else{
                        ESP_LOGW(TAG, "whitelist wrong format");
                        goto end;
                    }
                    if (strcmp(validValue, payload) == 0) {
                        char output_action[strlen(me_config.deviceName) + strlen(command) + 2];
                        sprintf(output_action, "%s/%s", me_config.deviceName, command);
                        //ESP_LOGD(TAG, "valid payload, execute:%s", output_action);
                        execute(output_action);
                        count++;
                    }
                }
                end:
                fclose(file);
                //ESP_LOGD(TAG, "File closed");
                if(count==0){
                    //char output_str = "/noMatches";
                    report("/noMatches", slot_num);
                }

            }
        }
    }
}

void start_whitelist_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "whitelist_task_%d", slot_num);
	xTaskCreatePinnedToCore(whitelist_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 20, NULL, 0);
    ESP_LOGD(TAG, "whitelist_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}



//---------------------------------COLLECTOR---------------------------------

void collector_task(void *arg) {
    #define MAX_LINE_LENGTH 256

    int slot_num = *(int*) arg;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    uint8_t stringMaxLenght = 7;
    if (strstr(me_config.slot_options[slot_num], "stringMaxLenght") != NULL) {
		stringMaxLenght = get_option_int_val(slot_num, "stringMaxLenght");
		ESP_LOGD(TAG, "Set stringMaxLenght:%d for slot:%d", stringMaxLenght, slot_num);
	}

    uint16_t waitingTime = 3000;//ms
    if (strstr(me_config.slot_options[slot_num], "waitingTime") != NULL) {
		waitingTime = get_option_int_val(slot_num, "waitingTime");
		ESP_LOGD(TAG, "Set waitingTime:%d for slot:%d", waitingTime, slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/collector_0")+3];
		sprintf(t_str, "%s/collector_%d",me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num]=strdup(t_str);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
	}


    uint string_lenght=0;
    char str[stringMaxLenght+1];
    memset(str, 0, sizeof(str));
    uint32_t dial_start_time = 0;
    uint8_t state_flag = 0;
    
    waitForWorkPermit(slot_num);

    while(1){
        vTaskDelay(15 / portTICK_PERIOD_MS);
        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload = NULL;
            char* cmd = msg.str+strlen(me_state.action_topic_list[slot_num])+1;
            if(strstr(cmd, ":")!=NULL){
                cmd = strtok_r(cmd, ":", &payload);
                //ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            }
            if(strstr(cmd, "clear")!=NULL){
                memset(str, 0, sizeof(str));
                string_lenght=0;
                state_flag = 0;
                ESP_LOGD(TAG, "strUpdate:%s", str);
            }else if(strstr(cmd, "add")!=NULL){
                if(payload!=NULL){
                    uint8_t len = strlen(payload);
                    if(len+string_lenght>stringMaxLenght){
                        len = stringMaxLenght-string_lenght;
                    }
                    strncat(str, payload, len);
                    string_lenght+=strlen(payload);
                    //ESP_LOGD(TAG, "strUpdate:%s", str);
                    if(state_flag == 0){
                        state_flag =1;
                    }
                    dial_start_time=pdTICKS_TO_MS(xTaskGetTickCount());

                }
            }  
        }

        if(state_flag == 1){
            if(((pdTICKS_TO_MS(xTaskGetTickCount())-dial_start_time)>=waitingTime)||(string_lenght>=stringMaxLenght)){
                //ESP_LOGD(TAG, "Input end, report number: %s", number_str);
                report(str, slot_num);
                memset(str, 0, stringMaxLenght);
                state_flag = 0;
                string_lenght = 0;
            }
        }

    }
}

void start_collector_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "collector_task_%d", slot_num);
	xTaskCreatePinnedToCore(collector_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 20, NULL, 0);
    ESP_LOGD(TAG, "collector_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


//------------------------SCALER-------------------------
//------------------------tankControl----------------------
void scaler_task(void* arg) {
    int slot_num = *(int*)arg;

    me_state.command_queue[slot_num] = xQueueCreate(50, sizeof(command_message_t));

    int16_t zeroDeadZone = 0;
	if (strstr(me_config.slot_options[slot_num], "zeroDeadZone") != NULL) {
		zeroDeadZone = get_option_int_val(slot_num, "zeroDeadZone");
		ESP_LOGD(TAG, "Set zeroDeadZone:%d for slot:%d",zeroDeadZone, slot_num);
	}
    

    int32_t inputMinVal = 0;
	if (strstr(me_config.slot_options[slot_num], "inputMinVal") != NULL) {
		inputMinVal = get_option_int_val(slot_num, "inputMinVal");
		ESP_LOGD(TAG, "Set inputMinVal:%ld for slot:%d",inputMinVal, slot_num);
	}
    int32_t inputMaxVal = 255;
	if (strstr(me_config.slot_options[slot_num], "inputMaxVal") != NULL) {
		inputMaxVal = get_option_int_val(slot_num, "inputMaxVal");
		ESP_LOGD(TAG, "Set inputMaxVal:%ld for slot:%d",inputMaxVal, slot_num);
	}
    // uint16_t inputMidlVal = (inputMaxVal-inputMinVal)/2+inputMinVal;
    // ESP_LOGD(TAG, "Set inputMidlVal:%d for slot:%d",inputMidlVal, slot_num);

    int32_t outputMinVal = 0;
	if (strstr(me_config.slot_options[slot_num], "outputMinVal") != NULL) {
		outputMinVal = get_option_int_val(slot_num, "outputMinVal");
		ESP_LOGD(TAG, "Set outputMinVal:%ld for slot:%d",outputMinVal, slot_num);
	}

    int32_t outputMaxVal = 255;
	if (strstr(me_config.slot_options[slot_num], "outputMaxVal") != NULL) {
		outputMaxVal = get_option_int_val(slot_num, "outputMaxVal");
		ESP_LOGD(TAG, "Set outputMaxVal:%ld for slot:%d",outputMaxVal, slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/scaler_0")+3];
		sprintf(t_str, "%s/scaler_%d",me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num]=strdup(t_str);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
	}

    waitForWorkPermit(slot_num);

    while(1){
        command_message_t cmd;
        if (xQueueReceive(me_state.command_queue[slot_num], &cmd, portMAX_DELAY) == pdPASS){
            char *command=cmd.str+strlen(me_state.action_topic_list[slot_num]);
            if(strstr(command, ":")==NULL){
                ESP_LOGE(TAG, "No arguments found. EXIT"); 
            }else{
                char *cmd_arg = strstr(command, ":")+1;
                int32_t inputVal = atoi(cmd_arg);
                if(inputVal<inputMinVal){
                    inputVal = inputMinVal;
                }else if(inputVal>inputMaxVal){
                    inputVal = inputMaxVal;
                }
                float inputFloat = (float)(inputVal-inputMinVal)/(inputMaxVal-inputMinVal);
                float outputVal = inputFloat*(outputMaxVal-outputMinVal)+outputMinVal;
                if(outputVal<outputMinVal){
                    outputVal = outputMinVal;
                }else if(outputVal>outputMaxVal){
                    outputVal = outputMaxVal;
                }
                if(abs(outputVal)<zeroDeadZone){
                    outputVal = 0;
                }
                ESP_LOGD(TAG, "SCALER inputVal:%ld, float:%f, outputVal:%ld", inputVal, inputFloat, (int32_t)outputVal);
                char str[50];
                memset(str, 0, sizeof(str));
                sprintf(str, "%ld", (int32_t)outputVal);
                report(str, slot_num);
            }
        }
        // не нужен, QueueRecive обеспечивает задержку 
        //vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void start_scaler_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "scaler_task_%d", slot_num);
	xTaskCreatePinnedToCore(scaler_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 12, NULL, 1);
    ESP_LOGD(TAG, "scaler_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

//------------------------tankControl----------------------
void tankControl_task(void* arg) {
    int slot_num = *(int*)arg;

    me_state.command_queue[slot_num] = xQueueCreate(50, sizeof(command_message_t));

    uint16_t deadBand = 10;
	if (strstr(me_config.slot_options[slot_num], "deadBand") != NULL) {
		deadBand = get_option_int_val(slot_num, "deadBand");
		ESP_LOGD(TAG, "Set deadBand:%d for slot:%d",deadBand, slot_num);
	}

    uint16_t inputMinVal = 0;
	if (strstr(me_config.slot_options[slot_num], "inputMinVal") != NULL) {
		inputMinVal = get_option_int_val(slot_num, "inputMinVal");
		ESP_LOGD(TAG, "Set inputMinVal:%d for slot:%d",inputMinVal, slot_num);
	}
    uint16_t inputMaxVal = 255;
	if (strstr(me_config.slot_options[slot_num], "inputMaxVal") != NULL) {
		inputMaxVal = get_option_int_val(slot_num, "inputMaxVal");
		ESP_LOGD(TAG, "Set inputMaxVal:%d for slot:%d",inputMaxVal, slot_num);
	}
    uint16_t inputMidlVal = (inputMaxVal-inputMinVal)/2+inputMinVal;
    ESP_LOGD(TAG, "Set inputMidlVal:%d for slot:%d",inputMidlVal, slot_num);

    uint16_t outputMaxVal = 255;
	if (strstr(me_config.slot_options[slot_num], "outputMaxVal") != NULL) {
		outputMaxVal = get_option_int_val(slot_num, "outputMaxVal");
		ESP_LOGD(TAG, "Set outputMaxVal:%d for slot:%d",outputMaxVal, slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/tankControl_0")+3];
		sprintf(t_str, "%s/tankControl_%d",me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num]=strdup(t_str);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
	}

    uint16_t accel =inputMaxVal/2;
    uint16_t steering = inputMaxVal/2;

    waitForWorkPermit(slot_num);

    while(1){
        command_message_t cmd;
        if (xQueueReceive(me_state.command_queue[slot_num], &cmd, portMAX_DELAY) == pdPASS){
            char *command=cmd.str+strlen(me_state.action_topic_list[slot_num])+1;
            if(strstr(command, ":")==NULL){
                ESP_LOGE(TAG, "No arguments found. EXIT"); 
            }else{
                char *cmd_arg = strstr(command, ":")+1;
                //ESP_LOGD(TAG, "Incoming command:%s  arg:%s", command, cmd_arg); 
                if(!memcmp(command, "accel", 5)){//------------------------------
                    accel = strtol(cmd_arg, NULL,10);
                    if(accel>inputMaxVal)accel=inputMaxVal;
                    if(accel<inputMinVal)accel=inputMinVal;
                    if(abs(accel-inputMidlVal)<deadBand)accel=inputMidlVal;
                    //accel = atof(cmd_arg);
                }else if(!memcmp(command, "steering", 8)){
                    steering = strtol(cmd_arg, NULL,10);
                    if(steering>inputMaxVal)steering=inputMaxVal;
                    if(steering<inputMinVal)steering=inputMinVal;
                    //if(abs(steering-inputMidlVal)<deadBand)steering=inputMidlVal;
                    //steering = atof(cmd_arg);
                }
                //ESP_LOGD(TAG, "Accel:%d Steering:%d", accel, steering);
                int16_t midSteering = steering - inputMidlVal;
                //float midSteering = (float)steering/(inputMidlVal*2) - 0.5;
                // if(midSteering>127)midSteering=127;
                // if(midSteering<-127)midSteering=-127;
                int16_t midAccel = accel - inputMidlVal;
                //float midAccel = (float)accel/(inputMidlVal*2) - 0.5;
                // if(midAccel>127)midAccel=127;
                // if(midAccel<-127)midAccel=-127;

                //ESP_LOGD(TAG, "Accel:%f Steering^%f", midAccel, midSteering);
                //ESP_LOGD(TAG, "Accel:%d Steering:%d", midAccel, midSteering);
                //ESP_LOGD(TAG, "Accel:%d Steering:%d", accel, steering);
                float ratio = (float)outputMaxVal/inputMaxVal;
                int leftSpeed = (int)((midAccel + midSteering)+inputMidlVal)*ratio;
                int rightSpeed = (int)((midAccel - midSteering)+inputMidlVal)*ratio;
                //int rightSpeed = (int)(((midAccel - midSteering)+0.5)*outputMaxVal);

                // Normalize speeds to 0.0 to 1.0 range
                // if(leftSpeed > 255) leftSpeed = 255;
                // if(leftSpeed < 0) leftSpeed = 0;
                // if(rightSpeed > 255) rightSpeed = 255;
                // if(rightSpeed < 0) rightSpeed = 0;
                //ESP_LOGD(TAG, "leftSpeed:%d rightSpeed:%d", leftSpeed, rightSpeed);
                
                
                char str[50];
                sprintf(str, "/ch_0:%d", leftSpeed);
                report(str, slot_num);
                memset(str, 0, sizeof(str));
                sprintf(str, "/ch_1:%d", rightSpeed);
                report(str, slot_num);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void start_tankControl_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "tankControl_task_%d", slot_num);
	xTaskCreatePinnedToCore(tankControl_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 12, NULL, 1);
    ESP_LOGD(TAG, "tankControl_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}