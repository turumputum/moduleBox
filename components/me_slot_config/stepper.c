#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "me_slot_config.h"
#include "driver/gptimer.h"
#include "driver/gpio.h"
#include <string.h>

#include "accelStepper.h"
#include "executor.h"

#include "reporter.h"
#include "stateConfig.h"


#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "STEPPER";

uint32_t testCount=0;

void stepper_set_targetPos(Stepper_t* motor, char* str){
    if(motor->init_state<0){
        return;
    }

    int32_t targetPos=0;

    if(strstr(str, ".")!=NULL){
        if(motor->_max_pos>0){
            float raw_input = atof(str);
            targetPos = raw_input*motor->_max_pos;
        }else{
            targetPos=0;
        }
        
    }else{
        if(strstr(str, "+")!=NULL){
            ESP_LOGD(TAG, "Add current pos");
            targetPos = currentPosition(motor)+atoi(str);
        }else if(strstr(str, "-")!=NULL){
            ESP_LOGD(TAG, "Minus current pos");
            targetPos = currentPosition(motor)+atoi(str);
        }else{
            targetPos = atoi(str);
        }
    }
    moveTo(motor,targetPos);
    ESP_LOGD(TAG, "Move to:%ld float input:%s", targetPos, str);
}

void stepper_stop(Stepper_t* motor){
    if(motor->init_state<0){
        return;
    }
    stop(motor);
}

void stepper_task(void *arg){
    Stepper_t stepper;

    char str[255];
    int slot_num = *(int*) arg;
	uint8_t dir_pin_num = SLOTS_PIN_MAP[slot_num][0];
    uint8_t step_pin_num = SLOTS_PIN_MAP[slot_num][1];

    uint8_t overRun_flag=0;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));
    
    InitStepper(&stepper, DRIVER, step_pin_num, dir_pin_num, 0);
	ESP_LOGD(TAG,"SETUP stepper dir_pin:%d step_pin:%d Slot:%d", dir_pin_num, step_pin_num, slot_num);
    //---
    stepper._dirInverted=0;
	if (strstr(me_config.slot_options[slot_num], "dirInverse")!=NULL){
		stepper._dirInverted=1;
        setPinsInvertedStpDir(&stepper, 1,0,0);
	}
    ESP_LOGD(TAG, "Set dir_inverse_val:%d Slot:%d", stepper._dirInverted, slot_num);
    //---
    int max_speed=100;
	if (strstr(me_config.slot_options[slot_num], "maxSpeed")!=NULL){
		max_speed = get_option_int_val(slot_num, "maxSpeed");
	}
    setMaxSpeed(&stepper,max_speed);
    ESP_LOGD(TAG, "Set max_speed:%d Slot:%d", max_speed, slot_num);//!!!!!!!!!!!!!!!!!!addd _cmin ptint
    //---
    int acceleration=100;
	if (strstr(me_config.slot_options[slot_num], "acceleration")!=NULL){
		acceleration = get_option_int_val(slot_num, "acceleration");
	}
    setAcceleration(&stepper, acceleration);
    ESP_LOGD(TAG, "Set acceleration:%d Slot:%d", acceleration, slot_num);
    //---
    stepper._max_pos=0;
    if (strstr(me_config.slot_options[slot_num], "maxPos")!=NULL){
		stepper._max_pos = get_option_int_val(slot_num, "maxPos");
        ESP_LOGD(TAG, "Set max position:%d Slot:%d", acceleration, slot_num);
	}
    
    //---sensor block---
    int sensor_up_inverse = 0;
    if (strstr(me_config.slot_options[slot_num], "sensorUpInverse")!=NULL){
		sensor_up_inverse = get_option_int_val(slot_num, "sensorUpInverse");
	}
    int sensor_down_inverse = 0;
    if (strstr(me_config.slot_options[slot_num], "sensorDownInverse")!=NULL){
		sensor_down_inverse = get_option_int_val(slot_num, "sensorDownInverse");
	}
    int sensor_slot=-1;
	if (strstr(me_config.slot_options[slot_num], "sensorSlot")!=NULL){
		sensor_slot = get_option_int_val(slot_num, "sensorSlot");
        ESP_LOGD(TAG, "Set sensor_slot:%d for Stepper on Slot:%d", sensor_slot, slot_num);
        set_stepper_sensor(&stepper,SLOTS_PIN_MAP[sensor_slot][0], sensor_up_inverse, SLOTS_PIN_MAP[sensor_slot][1], sensor_down_inverse);
	}
    int sensor_num=0;
    if (strstr(me_config.slot_options[slot_num], "sensorNum")!=NULL){
		sensor_num = get_option_int_val(slot_num, "sensorNum");
    }

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "stepper_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/stepper_")+3];
		sprintf(t_str, "%s/stepper_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart stepper_topic:%s", me_state.action_topic_list[slot_num]);
	}


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
            sprintf(str, "%s/stepper_%d:down sensor FAIL", me_config.deviceName, 0);
            report(str, 0);
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
            sprintf(str, "%s/stepper_%d:up sensor FAIL", me_config.deviceName, 0);
            report(str, 0);
        }
    }

    if (strstr(me_config.slot_options[slot_num], "stopOnSensor")!=NULL){
        stepper._stop_on_sensor=1;
    }else{
        stepper._stop_on_sensor=0;
    }
    setMaxSpeed(&stepper,max_speed);

    uint8_t flag_float_report=0;
    if (strstr(me_config.slot_options[slot_num], "floatReport")!=NULL){
        if(stepper._max_pos!=0){
            flag_float_report=1;
            ESP_LOGD(TAG, "Float report enabled");
        }
    }

    stepper.init_state=1;
    
    long prev_pos =currentPosition(&stepper);

    #define OVERRUN_DISTANCE 100000
    
    ESP_LOGD(TAG, "Stepper inited! lets work");
    while(1){
        vTaskDelay(pdMS_TO_TICKS(30));

        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload = NULL;
            char* cmd = msg.str;
            if(strstr(cmd, ":")!=NULL){
                cmd = strtok_r(msg.str, ":", &payload);
                ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            }else{
                ESP_LOGD(TAG, "Input command %s", cmd);
            }
            
            cmd = cmd + strlen(me_state.action_topic_list[slot_num]);
            if(strstr(cmd, "moveTo")!=NULL){
                overRun_flag=0;
                stepper_set_targetPos(&stepper, payload);
            }else if(strstr(cmd, "setMaxSpeed")!=NULL){
                setMaxSpeed(&stepper, atoi(payload));
            }else if(strstr(cmd, "setAcceleration")!=NULL){
                setAcceleration(&stepper, atoi(payload));
            }else if(strstr(cmd, "runSpeed")!=NULL){
                overRun_flag=1;
                int16_t speed=atoi(payload);
                int32_t tarPos;
                if(speed>0){
                    tarPos= OVERRUN_DISTANCE*2;
                }else{
                    tarPos=-OVERRUN_DISTANCE*2;
                }
                setMaxSpeed(&stepper, speed);
                moveTo(&stepper,tarPos);
            }else if(strstr(cmd, "stop")!=NULL){
                stop(&stepper);
            }
        }

        if(overRun_flag==1){
            if(currentPosition(&stepper)>=OVERRUN_DISTANCE){
                setCurrentPosition(&stepper, 0);
                if(maxSpeed>0){
                    moveTo(&stepper, OVERRUN_DISTANCE*2);
                }else{
                    moveTo(&stepper, -OVERRUN_DISTANCE*2);
                }
            }
        }

        if(currentPosition(&stepper) != prev_pos){
            //ESP_LOGD(TAG, "Distance to go:%ld speed:%ld delta:%d breakWay:%ld _n:%ld ,_cn:%ld", distanceToGo(&stepper), speed(&stepper), abs(currentPosition(&stepper)-prev_pos), stepper._break_way, stepper._n, stepper._cn);
            //ESP_LOGD(TAG, "_stepInterval:%ld _speed:%ld _cmin:%ld _n:%ld", stepper._stepInterval, stepper._speed, stepper._cmin,  stepper._n);
            prev_pos = currentPosition(&stepper);
            memset(str, 0, strlen(str));
			sprintf(str, "%ld", prev_pos);
			report(str, slot_num);

        }

        if(strlen(stepper.report_msg)>0){
            ESP_LOGD(TAG, "intMsg:%s", stepper.report_msg);
            memset(stepper.report_msg, 0, sizeof(stepper.report_msg));
        }

    }
}




void start_stepper_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_stepper_%d", slot_num);
	//xTaskCreate(stepper_task, tmpString, 1024*12, &t_slot_num,configMAX_PRIORITIES, NULL);
    xTaskCreatePinnedToCore(stepper_task, tmpString, 1024*12, &t_slot_num,configMAX_PRIORITIES-6, NULL,1);

	ESP_LOGD(TAG,"Stepper task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}