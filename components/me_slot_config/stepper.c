#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "me_slot_config.h"
#include "driver/gptimer.h"
#include "driver/gpio.h"
#include <string.h>

#include "accelStepper.h"

#include "reporter.h"
#include "stateConfig.h"


#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[6][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "STEPPER";

Stepper_t stepper;
uint32_t testCount=0;

void stepper_task(void *arg){
    char str[255];
    int num_of_slot = *(int*) arg;
	uint8_t dir_pin_num = SLOTS_PIN_MAP[num_of_slot][0];
    uint8_t step_pin_num = SLOTS_PIN_MAP[num_of_slot][1];
    
    InitStepper(&stepper, DRIVER, step_pin_num, dir_pin_num, 0);
	ESP_LOGD(TAG,"SETUP stepper dir_pin:%d step_pin:%d Slot:%d", dir_pin_num, step_pin_num, num_of_slot);
    //---
    stepper._dirInverted=0;
	if (strstr(me_config.slot_options[num_of_slot], "dir_inverse")!=NULL){
		stepper._dirInverted=1;
        setPinsInvertedStpDir(&stepper, 1,0,0);
	}
    ESP_LOGD(TAG, "Set dir_inverse_val:%d Slot:%d", stepper._dirInverted, num_of_slot);
    //---
    int max_speed=100;
	if (strstr(me_config.slot_options[num_of_slot], "max_speed")!=NULL){
		max_speed = get_option_int_val(num_of_slot, "max_speed");
	}
    setMaxSpeed(&stepper,max_speed);
    ESP_LOGD(TAG, "Set max_speed:%d Slot:%d", max_speed, num_of_slot);
    //---
    int acceleration=100;
	if (strstr(me_config.slot_options[num_of_slot], "acceleration")!=NULL){
		acceleration = get_option_int_val(num_of_slot, "acceleration");
	}
    setAcceleration(&stepper, acceleration);
    ESP_LOGD(TAG, "Set acceleration:%d Slot:%d", acceleration, num_of_slot);
    //---
    stepper._max_pos=0;
    if (strstr(me_config.slot_options[num_of_slot], "max_pos")!=NULL){
		stepper._max_pos = get_option_int_val(num_of_slot, "max_pos");
        ESP_LOGD(TAG, "Set max position:%d Slot:%d", acceleration, num_of_slot);
	}
    
    //---sensor block---
    int sensor_up_inverse = 0;
    if (strstr(me_config.slot_options[num_of_slot], "sensor_up_inverse")!=NULL){
		sensor_up_inverse = get_option_int_val(num_of_slot, "sensor_up_inverse");
	}
    int sensor_down_inverse = 0;
    if (strstr(me_config.slot_options[num_of_slot], "sensor_down_inverse")!=NULL){
		sensor_down_inverse = get_option_int_val(num_of_slot, "sensor_down_inverse");
	}
    int sensor_slot=-1;
	if (strstr(me_config.slot_options[num_of_slot], "sensor_slot")!=NULL){
		sensor_slot = get_option_int_val(num_of_slot, "sensor_slot");
        ESP_LOGD(TAG, "Set sensor_slot:%d for Stepper on Slot:%d", sensor_slot, num_of_slot);
        set_stepper_sensor(&stepper,SLOTS_PIN_MAP[sensor_slot][0], sensor_up_inverse, SLOTS_PIN_MAP[sensor_slot][1], sensor_down_inverse);
	}
    int sensor_num=0;
    if (strstr(me_config.slot_options[num_of_slot], "sensor_num")!=NULL){
		sensor_num = get_option_int_val(num_of_slot, "sensor_num");
    }

    char moveTo_trig[strlen(me_config.device_name)+18];
    sprintf(moveTo_trig,"%s/stepper_moveTo", me_config.device_name);
    me_state.action_topic_list[me_state.action_topic_list_index] = moveTo_trig;
    me_state.action_topic_list_index++;
    
    char setSpeed_trig[strlen(me_config.device_name)+20];
    sprintf(setSpeed_trig,"%s/stepper_setSpeed", me_config.device_name);
    me_state.action_topic_list[me_state.action_topic_list_index] = setSpeed_trig;
    me_state.action_topic_list_index++;
    
    char stop_trig[strlen(me_config.device_name)+15];
    sprintf(stop_trig,"%s/stepper_stop", me_config.device_name);
    me_state.action_topic_list[me_state.action_topic_list_index] = stop_trig;
    me_state.action_topic_list_index++;
    
    char setCurrentPos_trig[strlen(me_config.device_name)+25];
    sprintf(setCurrentPos_trig,"%s/stepper_setCurrentPos", me_config.device_name);
    me_state.action_topic_list[me_state.action_topic_list_index] = setCurrentPos_trig;
    me_state.action_topic_list_index++;

    char currentPos_action[strlen(me_config.device_name)+40];
    sprintf(currentPos_action,"%s/stepper_0", me_config.device_name);
    me_state.triggers_topic_list[me_state.triggers_topic_list_index] = currentPos_action;
    me_state.triggers_topic_list_index++;

    // homing procedure
    uint16_t tick=0;
    #define SEARCH_TIMEOUT 120
    if(sensor_num!=0){
        setMaxSpeed(&stepper,max_speed/10);
        if(gpio_get_level(stepper._pin_sensor_down)==!stepper._inv_sensor_down){
            ESP_LOGD(TAG, "Run out from sensor"); 
            moveTo(&stepper, 10000000);
            while(gpio_get_level(stepper._pin_sensor_down)==!stepper._inv_sensor_down){
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            moveTo(&stepper,currentPosition(&stepper)+stepper._break_way);
            while(distanceToGo(&stepper)>0){
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            ESP_LOGD(TAG, "Sensor inactive. Pos:%ld, tarPos:%ld", currentPosition(&stepper), targetPosition(&stepper));
        }
    }
    if(sensor_num>=1){
        ESP_LOGD(TAG, "Let's search down sensor");
        stepper._stop_on_sensor=1;
        moveTo(&stepper, -100000000);
        while((!stepper._down_sensor_found)&&(tick<=SEARCH_TIMEOUT)){
            vTaskDelay(pdMS_TO_TICKS(1000));
            tick++;
        }
        if(stepper._down_sensor_found==1){
            ESP_LOGD(TAG, "DOWN sensor reached");
            while(distanceToGo(&stepper)!=0){
              vTaskDelay(pdMS_TO_TICKS(10));  
            }
            setCurrentPosition(&stepper, 0);
        }else{
            memset(str, 0, strlen(str));
            sprintf(str, "%s/stepper_%d:down sensor FAIL", me_config.device_name, 0);
            report(str);
        }
    }
    if(sensor_num==2){
        ESP_LOGD(TAG, "Let's search up sensor");
        tick=0;
        moveTo(&stepper, 100000000);
        while((!stepper._up_sensor_found)&&(tick<=SEARCH_TIMEOUT)){
            vTaskDelay(pdMS_TO_TICKS(1000));
            tick++;
        }
        if(stepper._up_sensor_found==1){
            ESP_LOGD(TAG, "UP sensor reached");
            stepper._max_pos = currentPosition(&stepper);
        }else{
            memset(str, 0, strlen(str));
            sprintf(str, "%s/stepper_%d:up sensor FAIL", me_config.device_name, 0);
            report(str);
        }
    }

    if (strstr(me_config.slot_options[num_of_slot], "stop_on_sensor")!=NULL){
        stepper._stop_on_sensor=1;
    }else{
        stepper._stop_on_sensor=0;
    }
    setMaxSpeed(&stepper,max_speed);

    uint8_t flag_float_report=0;
    if (strstr(me_config.slot_options[num_of_slot], "float_report")!=NULL){
        if(stepper._max_pos!=0){
            flag_float_report=1;
            ESP_LOGD(TAG, "Float report enabled");
        }
    }

    stepper.init_state=1;
    
    long prev_pos =currentPosition(&stepper);
    while(1){
        vTaskDelay(pdMS_TO_TICKS(22));

        if(currentPosition(&stepper) != prev_pos){
            //ESP_LOGD(TAG, "Distance to go:%ld speed:%ld delta:%d breakWay:%ld _n:%ld ,_cn:%ld", distanceToGo(&stepper), speed(&stepper), abs(currentPosition(&stepper)-prev_pos), stepper._break_way, stepper._n, stepper._cn);
            
            prev_pos = currentPosition(&stepper);
            memset(str, 0, strlen(str));
            if(flag_float_report){
                sprintf(str, "%s/stepper_%d:%f", me_config.device_name, 0, (float)currentPosition(&stepper)/stepper._max_pos);
            }else{
                sprintf(str, "%s/stepper_%d:%ld", me_config.device_name, 0, currentPosition(&stepper));
            }
            report(str);
        }

        if(strlen(stepper.report_msg)>0){
            ESP_LOGD(TAG, "%s", stepper.report_msg);
            memset(stepper.report_msg, 0, sizeof(stepper.report_msg));
        }

    }
}

void stepper_set_targetPos(char* str){
    if(stepper.init_state<0){
        return;
    }

    float raw_input = 0.0;
    int32_t targetPos=0;

    if(strstr(str, ".")!=NULL){
        raw_input = atof(str);
        targetPos = raw_input*stepper._max_pos;
    }else{
        if(strstr(str, "+")!=NULL){
            targetPos = currentPosition(&stepper)+atoi(str);
        }else if(strstr(str, "-")!=NULL){
            targetPos = currentPosition(&stepper)-atoi(str);
        }else{
            targetPos = atoi(str);
        }
    }
    moveTo(&stepper,targetPos);
    ESP_LOGD(TAG, "Move to:%ld float input:%s", targetPos, str);
}

void stepper_set_speed(int32_t speed){
    if(stepper.init_state<0){
        return;
    }
    ESP_LOGD(TAG, "Set speed to:%ld", speed);
    setMaxSpeed(&stepper,speed);
}

void stepper_set_currentPos(int32_t val){
    if(stepper.init_state<0){
        return;
    }
    ESP_LOGD(TAG, "Set current pos to:%ld", val);
    setCurrentPosition(&stepper, val);
}

void stepper_stop_on_sensor(int8_t val){
    if(stepper.init_state<0){
        return;
    }
    ESP_LOGD(TAG,"Stepper stop on sensoer val:%d",val);
    stepper._stop_on_sensor=val;
}

void stepper_stop(void){
    if(stepper.init_state<0){
        return;
    }
    stop(&stepper);
}


void start_stepper_task(int num_of_slot){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = num_of_slot;
	char tmpString[60];
	sprintf(tmpString, "task_stepper_%d", num_of_slot);
	xTaskCreate(stepper_task, tmpString, 1024*12, &t_slot_num,12, NULL);

	ESP_LOGD(TAG,"Stepper task created for slot: %d Heap usage: %lu free heap:%u", num_of_slot, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}