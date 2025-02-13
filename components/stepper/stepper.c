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

#include "asyncStepper.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "STEPPER";

uint32_t testCount=0;

gptimer_handle_t usTimer;
uint8_t usTimer_flag=0;

// void startUsTimer(void){
//     if(usTimer_flag==0){
// 		gptimer_config_t time_config = {
// 			.clk_src = GPTIMER_CLK_SRC_DEFAULT,
// 			.direction = GPTIMER_COUNT_UP,
// 			.resolution_hz = 1000000, // 1MHz, 1 tick=1us
// 		};
// 		ESP_ERROR_CHECK(gptimer_new_timer(&time_config, &usTimer));
// 		ESP_ERROR_CHECK(gptimer_enable(usTimer));
// 		ESP_ERROR_CHECK(gptimer_start(usTimer));
// 		usTimer_flag=1;
// 	}
// }

// static float convert_to_smooth_freq(uint32_t freq1, uint32_t freq2, uint32_t freqx){
//     float normalize_x = ((float)(freqx - freq1)) / (freq2 - freq1);
//     // third-order "smoothstep" function: https://en.wikipedia.org/wiki/Smoothstep
//     float smooth_x = normalize_x * normalize_x * (3 - 2 * normalize_x);
//     return smooth_x * (freq2 - freq1) + freq1;
// }

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