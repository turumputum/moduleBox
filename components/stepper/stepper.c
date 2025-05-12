#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "me_slot_config.h"
#include "driver/gptimer.h"
#include <esp_timer.h>
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include <string.h>
#include "driver/rmt_tx.h"
//#include "accelStepper.h"
#include "executor.h"
#include "stepper.h"

#include "reporter.h"
#include "stateConfig.h"

#include "stepper_motor_encoder.h"

#include "esp_log.h"
#include "me_slot_config.h"
#include "sCurveStepper.h"
#include "asyncStepper.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "STEPPER";

//uint32_t testCount=0;
// gptimer_handle_t usTimer;
// uint8_t usTimer_flag=0;

#define ACC 1
#define DEC -1

#define UP 1
#define DOWN -1

void stepperSpeed_task(void *arg){
   
    char str[255];
    int slot_num = *(int*) arg;
	uint8_t dir_pin_num = SLOTS_PIN_MAP[slot_num][0];
    uint8_t step_pin_num = SLOTS_PIN_MAP[slot_num][1];

    speedStepper_t stepper = SPEED_STEPPER_DEFAULT();

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
    
	ESP_LOGD(TAG,"SETUP stepper dir_pin:%d step_pin:%d Slot:%d", dir_pin_num, step_pin_num, slot_num);
    //---
    stepper._dirInverse=0;
	if (strstr(me_config.slot_options[slot_num], "dirInverse")!=NULL){
		stepper._dirInverse=1;
	}
    ESP_LOGD(TAG, "Set dir_inverse_val:%d Slot:%d", stepper._dirInverse, slot_num);

    //---
    int acceleration=100;
	if (strstr(me_config.slot_options[slot_num], "acceleration")!=NULL){
		acceleration = get_option_int_val(slot_num, "acceleration");
	}
    ESP_LOGD(TAG, "Set acceleration:%d Slot:%d", acceleration, slot_num);
    //---
    uint8_t pulseWidth=10;
	if (strstr(me_config.slot_options[slot_num], "pulseWidth")!=NULL){
		pulseWidth = get_option_int_val(slot_num, "pulseWidth");
	}
    ESP_LOGD(TAG, "Set pulseWidth:%d Slot:%d", pulseWidth, slot_num);
    
    int period=25;

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

    
    speedStepper_init(&stepper, step_pin_num, dir_pin_num, pulseWidth);

    //float startFreq = 0;
    float currentFreq = 0;
    float targetFreq = 0;
    float accStep = acceleration * period / 1000.0f;;
    int8_t prevDir = 0;

    TickType_t lastWakeTime = xTaskGetTickCount();

    while(1){
        if(fabs(targetFreq - currentFreq) > 0.0001){
            
            float opTargetFreq = targetFreq;//временная целевая скорость
            //проверяем, надо ли переключить направление
            if(((currentFreq>0)&&(targetFreq<0))||((currentFreq<0)&&(targetFreq>0))){
                opTargetFreq = 0;
            }

            if(opTargetFreq > currentFreq){
                currentFreq += accStep;
                if(currentFreq > opTargetFreq){
                    currentFreq = opTargetFreq;
                }
            }else{
                currentFreq -= accStep;
                if(currentFreq < opTargetFreq){
                    currentFreq = opTargetFreq;
                }
            }

            // updateSCurveSpeed(&stepper, period);
            // setSpeed(&stepper, stepper._currentSpeed);
            //printf(" %f;", freq);
            if((stepper._state == SPEED_STEPPER_STOP)&&(fabs(targetFreq)>0.0001)){
                speedStepper_start(&stepper);
                stepper._state = SPEED_STEPPER_RUN;
            }

            if((stepper._state == SPEED_STEPPER_RUN)&&(fabs(currentFreq)<1)){
                speedStepper_stop(&stepper);
                stepper._state = SPEED_STEPPER_STOP;
                currentFreq = 0;
            }

            speedStepper_setSpeed(&stepper, (int32_t)currentFreq);
            int8_t dir = (currentFreq >= 0) ? UP : DOWN;
            if(dir != prevDir){
                speedStepper_setDirection(&stepper, dir);
                prevDir = dir;
            }
            
            //ESP_LOGD(TAG, "Stepper %d curentFreq:%f opTarget:%f", slot_num, currentFreq, opTargetFreq);
        }else{
            currentFreq = targetFreq;
        }
        
        
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
            if(strstr(cmd, "runSpeed")!=NULL){
                int32_t val = atoi(payload);
                targetFreq = val;
            }else if(strstr(cmd, "stop")!=NULL){
                targetFreq = 0;
                // stopStepper(&stepper);
            }
        }
        vTaskDelayUntil(&lastWakeTime, period);
    }

}

void start_stepperSpeed_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_stepperSpeed_%d", slot_num);
	//xTaskCreate(stepper_task, tmpString, 1024*12, &t_slot_num,configMAX_PRIORITIES, NULL);
    xTaskCreatePinnedToCore(stepperSpeed_task, tmpString, 1024*12, &t_slot_num,configMAX_PRIORITIES-6, NULL,1);

	ESP_LOGD(TAG,"Stepper task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}



//---------------------------stepper------------------------------


void getHomingSenesorState(int slot_num, int* state){
    command_message_t msg;
    if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
        char* payload = NULL;
        char* cmd = msg.str;
        if(strstr(cmd, ":")!=NULL){
            cmd = strtok_r(msg.str, ":", &payload);
            ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            cmd = cmd + strlen(me_state.action_topic_list[slot_num]);
            if(strstr(cmd, "homingSensor")!=NULL){
                *state = atoi(payload);
            }
        }
    }
}

void stepper_task(void *arg){
   
    char str[255];
    int slot_num = *(int*) arg;
	uint8_t dir_pin_num = SLOTS_PIN_MAP[slot_num][0];
    uint8_t step_pin_num = SLOTS_PIN_MAP[slot_num][1];

    stepper_t stepper = STEPPER_DEFAULT();
    stepper.stepPin=step_pin_num;
    stepper.dirPin=dir_pin_num;

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
    
	ESP_LOGD(TAG,"SETUP stepper dir_pin:%d step_pin:%d Slot:%d", dir_pin_num, step_pin_num, slot_num);
    //---
    stepper.dirInverse=0;
	if (strstr(me_config.slot_options[slot_num], "dirInverse")!=NULL){
		stepper.dirInverse=1;
	}
    ESP_LOGD(TAG, "Set dir_inverse_val:%d Slot:%d", stepper.dirInverse, slot_num);

    //---
	if (strstr(me_config.slot_options[slot_num], "accel")!=NULL){
		stepper.accel = get_option_int_val(slot_num, "accel");
	}
    ESP_LOGD(TAG, "Set acceleration:%ld Slot:%d", stepper.accel, slot_num);

    //---
    int8_t posReport_flag=0;
	if (strstr(me_config.slot_options[slot_num], "posReport")!=NULL){
		posReport_flag = 1;
	}

    //---
    int8_t speedReport_flag=0;
	if (strstr(me_config.slot_options[slot_num], "speedReport")!=NULL){
		speedReport_flag = 1;
	}

    //---
    int32_t maxSpeed = 100;
	if (strstr(me_config.slot_options[slot_num], "maxSpeed")!=NULL){
		maxSpeed = get_option_int_val(slot_num, "maxSpeed");
        stepper.maxSpeed = maxSpeed;
	}
    ESP_LOGD(TAG, "Set maxSpeed:%ld Slot:%d", stepper.maxSpeed, slot_num);

    uint16_t refreshPeriod = 10;
    if (strstr(me_config.slot_options[slot_num], "refreshPeriod") != NULL) {
		refreshPeriod = (get_option_int_val(slot_num, "refreshPeriod"));
		ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",refreshPeriod, slot_num);
	}

    int8_t homingDir = 0;
    if (strstr(me_config.slot_options[slot_num], "homingDir")!=NULL){
		char *tmpStr;
        tmpStr = get_option_string_val(slot_num, "homingDir");
        if(strcmp(tmpStr, "up")==0){
            homingDir = UP;
        }else if(strcmp(tmpStr, "down")==0){
            homingDir = DOWN;
        }
        ESP_LOGD(TAG, "Set homingDir:%d Slot:%d", homingDir, slot_num);
	}

    //---
    uint32_t homingSpeed = stepper.maxSpeed/5;
    if (strstr(me_config.slot_options[slot_num], "homingSpeed")!=NULL){
		homingSpeed = abs(get_option_int_val(slot_num, "homingSpeed"));
	}
    ESP_LOGD(TAG, "Set homingSpeed:%ld Slot:%d", homingSpeed, slot_num);
    

    //---
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

    stepper_init(&stepper, step_pin_num, dir_pin_num, 10);
    int32_t prevPos=0;
    int32_t prevSpeed=0;
    TickType_t lastWakeTime = xTaskGetTickCount();

    //-----------homing--------------
    if(homingDir!=0){
        stepper.maxSpeed = homingSpeed;
        int sensorState = 0;
        vTaskDelay(pdMS_TO_TICKS(2000));
        getHomingSenesorState(slot_num, &sensorState);
        if(sensorState==1){
            while(sensorState!=0){
                //---sensor is active run away---
                ESP_LOGD(TAG, "Homing sensor active run away");
                stepper_moveTo(&stepper, stepper.currentPos + 100*homingDir*(-1));
                while(stepper.currentPos!=stepper.targetPos){
                    stepper_speedUpdate(&stepper, refreshPeriod);
                    // pcnt_unit_get_count(stepper.pcntUnit, &stepper.currentPos);
                    // ESP_LOGD(TAG, "Wait move done currentPos:%ld targetPos:%ld", stepper.currentPos, stepper.targetPos);
                    getHomingSenesorState(slot_num, &sensorState);
                    vTaskDelayUntil(&lastWakeTime, refreshPeriod);
                }
                ESP_LOGD(TAG, "moveEnd sensorIs:%d", sensorState);
            }
            stepper_setZero(&stepper);
            stepper_stop(&stepper);
        }
        ESP_LOGD(TAG, "Lets search sensor");
        stepper_moveTo(&stepper, INT32_MAX*homingDir);
        while(sensorState!=1){
            stepper_speedUpdate(&stepper, refreshPeriod);
            getHomingSenesorState(slot_num, &sensorState);
            //ESP_LOGD(TAG, "sensorState:%d", sensorState);
            vTaskDelayUntil(&lastWakeTime, refreshPeriod);
        }
        stepper_setZero(&stepper);
        stepper_stop(&stepper);
        report("/homeFound",slot_num);
        stepper.maxSpeed=maxSpeed;
        ESP_LOGD(TAG, "----------------Homing done-----------------");
    }

    int32_t prevState=0;
   
    while(1){
        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload = NULL;
            char* cmd = msg.str;
            if(strstr(cmd, ":")!=NULL){
                cmd = strtok_r(msg.str, ":", &payload);
                ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
                cmd = cmd + strlen(me_state.action_topic_list[slot_num]);
                if(strstr(cmd, "moveTo")!=NULL){
                    int32_t val = atoi(payload);
                    stepper_moveTo(&stepper, val);
                }else if(strstr(cmd, "runSpeed")!=NULL){
                    stepper.maxSpeed = atoi(payload);
                    stepper_moveTo(&stepper, stepper.maxSpeed>0?(INT32_MAX-1):(INT32_MIN+1));
                    stepper.runSpeedFlag = 1;
                    ESP_LOGD(TAG, "Run speed:%ld", stepper.maxSpeed);
                }else if(strstr(cmd, "setAccel")!=NULL){
                    stepper.accel = atoi(payload);
                    ESP_LOGD(TAG, "Set accel:%ld", stepper.accel);
                }else if(strstr(cmd, "setMaxSpeed")!=NULL){
                    stepper.maxSpeed = atoi(payload);
                    ESP_LOGD(TAG, "Set maxSpeed:%ld", stepper.maxSpeed);
                } 
            }else{
                ESP_LOGD(TAG, "Input command %s", cmd);
            }
            if(strstr(cmd, "stop")!=NULL){
                stepper_stop(&stepper);
            }
            
            
        }
        stepper_speedUpdate(&stepper, refreshPeriod);
        stepper_getCurrentPos(&stepper);

        if(posReport_flag){
            if(stepper.currentPos!=prevPos){
                char str[15];
                sprintf(str, "/pos:%ld", stepper.currentPos);
                report(str,slot_num);
                //ESP_LOGD(TAG, "Stepper_%d curentPos:%s", slot_num, str);
                prevPos=stepper.currentPos;
            }
        }

        if(speedReport_flag){
            if(prevSpeed!=stepper.currentSpeed){
                char str[15];
                sprintf(str, "/speed:%ld", stepper.currentSpeed);
                report(str,slot_num);
                //ESP_LOGD(TAG, "Stepper_%d curentSpeed:%s", slot_num, str);
                prevSpeed=stepper.currentSpeed;
            }
        }

        vTaskDelayUntil(&lastWakeTime, refreshPeriod);
    }

}



void start_stepper_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_stepper_%d", slot_num);
	//xTaskCreate(stepper_task, tmpString, 1024*12, &t_slot_num,configMAX_PRIORITIES, NULL);
    xTaskCreatePinnedToCore(stepper_task, tmpString, 1024*12, &t_slot_num,configMAX_PRIORITIES-6, NULL,1);

	ESP_LOGD(TAG,"stepper task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}