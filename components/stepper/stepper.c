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

#include "esp_log.h"
#include "me_slot_config.h"

#include "asyncStepper.h"

#include <stdcommand.h>
#include <stdreport.h>

#include <manifest.h>
#include <mbdebug.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "STEPPER";


//---------------------------stepper------------------------------
#define NOT_HOMED 0
#define HOMING 0
#define IDLE 1
#define RUN_POS 2
#define RUN_SPEED 3
#define GOING_HOME 4

#define CW 1
#define CCW -1


typedef struct __tag_STEPPERCONFIG{
	uint8_t 				state;
	int 				    dir;
	uint32_t 				accel;
	uint32_t 				maxSpeed;
	uint16_t                refreshPeriod;
	int					    homingDir;
    int                     homingSensorState;
    int32_t                 currentPos;
    int32_t                 targetPos;
	uint32_t				homingSpeed;
	int32_t				    maxVal;
    int32_t                 minVal;
	int 					speedReportFlag;
	int 					posReportFlag;
    int 					circularCounterFlag;
    int                     goHomeOnStart;

    STDCOMMANDS             cmds;

	int 					posReport;
	int						speedReport;
	int 					homeReport;
} STEPPERCONFIG, * PSTEPPERCONFIG; 

typedef enum
{
	stepCMD_goHome = 0,
	stepCMD_moveToAbs,
    stepCMD_moveToInc,
	stepCMD_runSpeed,
    stepCMD_setMaxSpeed,
    stepCMD_setAccel,
    stepCMD_stop,
	stepCMD_break,
    stepCMD_setHomingSensor
} stepCMD;

/*
    Модуль управления шаговым двигателем сигналами step/dir
*/
void configure_stepper(PSTEPPERCONFIG c, int slot_num){
    stdcommand_init(&c->cmds, slot_num);

    c->dir = CW;
    /* Флаг инвертирует направление работы двигателя
	*/
	c->dir = get_option_flag_val(slot_num, "dirInverse") ? CCW : CW;
	ESP_LOGD(TAG, "[stepper_%d] dir:%s", slot_num, (c->dir == CW)? "CW" : "CCW");

    /* Флаг включает рапорты по положению
	*/
	c->posReportFlag = get_option_flag_val(slot_num, "posReport");
	if(c->posReportFlag){
        ESP_LOGD(TAG, "[stepper_%d] posReport enable", slot_num);
    }

    /* Флаг включает рапорты по скорости
	*/
	c->circularCounterFlag = get_option_flag_val(slot_num, "circularCounter");
	if(c->circularCounterFlag){
        ESP_LOGD(TAG, "[stepper_%d] circularCounter enable", slot_num);
    }

    /* Флаг базирует двигатель сразу после включения
        иначе, слот ждет команды "goHome" из вне
	*/
	c->goHomeOnStart = get_option_flag_val(slot_num, "goHomeOnStart");
	if(c->goHomeOnStart){
        ESP_LOGD(TAG, "[stepper_%d] goHomeOnStart enable", slot_num);
    }

    /* Флаг влючает режим кругового счетчика
	*/
	c->speedReportFlag = get_option_flag_val(slot_num, "speedReport");
	if(c->speedReportFlag){
        ESP_LOGD(TAG, "[stepper_%d] speedReport enable", slot_num);
    }


    c->state=NOT_HOMED;
    c->homingDir = 0;
    /* задает направление поска домашней позиции
        - по умочанию равен 0, поиск домашне позиции отключен
    */
    if ((c->homingDir = get_option_enum_val(slot_num, "homingDir","", "CW", "CCW", NULL)) < 0){
        
    }
    if(c->homingDir==1){
        c->homingDir = CW;
        ESP_LOGD(TAG, "[stepper_%d] homing dir:CW", slot_num);
    }else if(c->homingDir==2){
        c->homingDir = CCW;
        ESP_LOGD(TAG, "[stepper_%d] homing dir:CCW", slot_num);
    }else{
        c->state=IDLE;
        ESP_LOGD(TAG, "[stepper_%d] homing procedure disabled", slot_num);
    }



    /* Ускорение и замедление
    - шагов в секунду в квадрате
	*/
	c->accel =  get_option_int_val(slot_num, "accel", "step/sek^2", 100, 1, UINT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] accel:%ld", slot_num, c->accel);

    /* Максимальная скорость 
    - шагов в секунду
	*/
	c->maxSpeed =  get_option_int_val(slot_num, "maxSpeed", "step/sek", 100, 1, UINT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] maxSpeed:%ld", slot_num, c->maxSpeed); 
    
    /* Частота обновления
    - раз в секунду
	*/
	c->refreshPeriod =  1000/get_option_int_val(slot_num, "refreshRate", "fps", 20, 1, 100);
    ESP_LOGD(TAG, "[stepper_%d] refreshPeriod:%d", slot_num, c->refreshPeriod);

    /* Скорость базирование
    - шагов в секунду
	*/
    c->homingSpeed = c->maxSpeed / 4;
	c->homingSpeed =  get_option_int_val(slot_num, "homingSpeed", "step/sek", 100, 1, 100);
    ESP_LOGD(TAG, "[stepper_%d] homingSpeed:%ld", slot_num, c->homingSpeed);

    /* Максимальное значение положения
    - шагов
	*/
	c->maxVal =  get_option_int_val(slot_num, "maxVal", "step", INT32_MAX, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] maxVal:%ld", slot_num, c->maxVal);

    /* Минимальное значение положения
    - шагов
	*/
	c->minVal =  get_option_int_val(slot_num, "minVal", "step", INT32_MIN, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] minVal:%ld", slot_num, c->minVal);

    // не стандартный топик
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic", "/stepper_0");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "stepper_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/stepper_0")+3];
		sprintf(t_str, "%s/stepper_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart stepper_topic:%s", me_state.action_topic_list[slot_num]);
	}

    /* Рапортует текущее положение
    - шаг
	*/
	c->posReport = stdreport_register(RPTT_string, slot_num, "step", "pos");

    /* Рапортует текущую скорость
    - шаг\сек
	*/
	c->speedReport = stdreport_register(RPTT_string, slot_num, "step/sek", "speed");

    /* Рапортует текущий режим базирования
	*/
	c->homeReport = stdreport_register(RPTT_string, slot_num, "", "homingState");

    /* Команда запускает процесс базирования
    */
    stdcommand_register(&c->cmds, stepCMD_goHome, "goHome", PARAMT_none);

    /* Команда включает, режим управления по положению и устанавливает целевое значение в абсолютном режиме
    */
    stdcommand_register(&c->cmds, stepCMD_moveToAbs, "moveToAbs", PARAMT_int);

    /* Команда включает, режим управления по положению и устанавливает целевое значение в виде приращения
    */
    stdcommand_register(&c->cmds, stepCMD_moveToInc, "moveToInc", PARAMT_int);

    /* Команда включает, режим управления по скорости и устанавливает максимальную скорость движения мотора
    */
    stdcommand_register(&c->cmds, stepCMD_runSpeed, "runSpeed", PARAMT_int);

    /* Команда устанавливает максимальную скорость движения мотора
    */
    stdcommand_register(&c->cmds, stepCMD_setMaxSpeed, "setMaxSpeed", PARAMT_int);

    /* Команда устанавливает максимальную скорость движения мотора
    */
    stdcommand_register(&c->cmds, stepCMD_setAccel, "setAccel", PARAMT_int);

    /* Команда экстренной остановки
    */
    stdcommand_register(&c->cmds, stepCMD_stop, "stop", PARAMT_none);

    /* Команда остановки с учетом пути торможения
    */
    stdcommand_register(&c->cmds, stepCMD_break, "break", PARAMT_none);

    c->homingSensorState=-1;
    /* Команда установки значения датчика нулевого положения
    */
    stdcommand_register(&c->cmds, stepCMD_setHomingSensor, "setHomingSensor", PARAMT_int);

}


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

#define HOMING_WAITING 1
#define HOMING_TO_SENSOR 2 
#define HOMING_OUT_SENSOR 3

void stepper_task(void *arg){
    PSTEPPERCONFIG c = calloc(1, sizeof(STEPPERCONFIG));
    STDCOMMAND_PARAMS       params = { 0 };

    char str[255];
    int slot_num = *(int*) arg;
	
    configure_stepper(c, slot_num);
    c->homingSensorState = -1;

    stepper_t stepper = STEPPER_DEFAULT();
    stepper.dirPin = SLOTS_PIN_MAP[slot_num][0];
    stepper.stepPin = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
    
    stepper.accel = c->accel;
    stepper.maxSpeed = c->maxSpeed;

	stepper_init(&stepper, stepper.stepPin, stepper.dirPin, 10);
    
    int32_t prevState=0;
    int32_t prevPos=0;
    int32_t prevSpeed=0;

    int homingProcedureState = HOMING_WAITING;

    waitForWorkPermit(slot_num);

    if(c->homingDir==0){
        stdreport_s(c->homeReport, "disable");
    }else{
        if(c->goHomeOnStart){
            c->state=GOING_HOME;
        }else{
            stdreport_s(c->homeReport, "waitingCommand"); 
        }
    }
    

    TickType_t lastWakeTime = xTaskGetTickCount();
    int32_t target=0;

    while(1){

        int cmd = stdcommand_receive(&c->cmds, &params, 5);
		char * cmd_arg = (params.count > 0) ? params.p[0].p : (char *)"0";
        
        switch (cmd){
            case -1: // none
                break;

            case stepCMD_goHome:
                c->state = GOING_HOME;
                ESP_LOGD(TAG, "[stepper_%d] lets go home", slot_num);
                break;

            case stepCMD_moveToInc:
                // if((c->state==NOT_HOMED)||(c->state==GOING_HOME)){
                //     break; 
                // }
                c->state = RUN_POS;
                stepper.runSpeedFlag = 0;
                target=0;
                if(cmd_arg[0]==45){
                    target = stepper.currentPos - atoi(cmd_arg+1);
                }else{
                    target = stepper.currentPos + atoi(cmd_arg); 
                }
                stepper_moveTo(&stepper, target);
                ESP_LOGD(TAG, "[stepper_%d] moveTo:%ld", slot_num, target);
                break;

            case stepCMD_moveToAbs:
                // if((c->state==NOT_HOMED)||(c->state==GOING_HOME)){
                //     break; 
                // }
                c->state = RUN_POS;
                stepper.runSpeedFlag = 0;
                target=atoi(cmd_arg);
                stepper_moveTo(&stepper, target);
                ESP_LOGD(TAG, "[stepper_%d] moveTo:%ld", slot_num, target);
                break;

            case stepCMD_runSpeed:
                // if((c->state==NOT_HOMED)||(c->state==GOING_HOME)){
                //     break; 
                // }
                c->state = RUN_SPEED;
                stepper.runSpeedFlag = 1;
                stepper.maxSpeed = atoi(cmd_arg);
                stepper_moveTo(&stepper, stepper.maxSpeed>0?(INT32_MAX-1):(INT32_MIN+1));
                ESP_LOGD(TAG, "[stepper_%d] runSpeed%ld", slot_num, stepper.maxSpeed);
                break;

            case stepCMD_setMaxSpeed:
                stepper.maxSpeed = atoi(cmd_arg);
                ESP_LOGD(TAG, "[stepper_%d] set maxSpeed:%ld", slot_num, stepper.maxSpeed);
                break;

            case stepCMD_setAccel:
                stepper.accel = atoi(cmd_arg);
                ESP_LOGD(TAG, "[stepper_%d] set accel:%ld", slot_num, stepper.accel);
                break;

            case stepCMD_stop:
                stepper_stop(&stepper);
                ESP_LOGD(TAG, "[stepper_%d] STOP", slot_num);
                if(c->state==GOING_HOME){
                    c->state=NOT_HOMED;
                }
                break;

            case stepCMD_break:
                stepper_break(&stepper);
                ESP_LOGD(TAG, "[stepper_%d] break", slot_num);
                break;

            case stepCMD_setHomingSensor:
                c->homingSensorState = atoi(cmd_arg);
                ESP_LOGD(TAG, "[stepper_%d] setHomingSensor:%d", slot_num, c->homingSensorState);
                break;
        }

        if(c->state==GOING_HOME){
            if(homingProcedureState==HOMING_WAITING){
                stdreport_s(c->homeReport, "homing");
                stepper.maxSpeed = c->homingSpeed;
                stepper.accel = stepper.maxSpeed*2;

                if(c->homingSensorState==1){
                    homingProcedureState = HOMING_OUT_SENSOR;
                    stepper_moveTo(&stepper,(c->homingDir) ? INT32_MIN : INT32_MAX);
                    ESP_LOGD(TAG, "[stepper_%d] homing out of sensor", slot_num);
                }else{
                    homingProcedureState = HOMING_TO_SENSOR; 
                    stepper_moveTo(&stepper, (c->homingDir) ? INT32_MAX : INT32_MIN);
                    ESP_LOGD(TAG, "[stepper_%d] homing to sensor", slot_num);
                }
            }else if(homingProcedureState == HOMING_OUT_SENSOR){
                if(c->homingSensorState==0){
                    ESP_LOGD(TAG, "[stepper_%d] sensor reseted, homing again", slot_num);
                    vTaskDelay(500);
                    homingProcedureState = HOMING_TO_SENSOR; 
                    stepper_moveTo(&stepper, (c->homingDir) ? INT32_MAX : INT32_MIN);
                }
            }else if(homingProcedureState == HOMING_TO_SENSOR){
                if(c->homingSensorState==1){
                    stepper_setZero(&stepper);
                    stepper_break(&stepper);
                    //stepper_stop(&stepper);
                    stepper.maxSpeed = c->maxSpeed;
                    stepper.accel = c->accel;
                    c->state=IDLE;
				    stdreport_s(c->homeReport, "done");
                }
            }
        }

        stepper_speedUpdate(&stepper, c->refreshPeriod);
        stepper_getCurrentPos(&stepper);
        //ESP_LOGD(TAG, "currentPos: %ld prevPos:%ld dir:%d", stepper.currentPos,  stepper.pcnt_prevPos,  stepper.dir);

        if(c->posReportFlag){
            if(stepper.currentPos!=prevPos){
                char str[15];
                sprintf(str, "%ld", stepper.currentPos);
				stdreport_s(c->posReport, str);
                //ESP_LOGD(TAG, "Stepper_%d curentPos:%s", slot_num, str);
                prevPos=stepper.currentPos;
            }
        }

        if(c->speedReportFlag){
            if(prevSpeed!=stepper.currentSpeed){
                char str[15];
                sprintf(str, "%ld", stepper.currentSpeed);
                stdreport_s(c->speedReport, str);
                //report(str,slot_num);
                //ESP_LOGD(TAG, "Stepper_%d curentSpeed:%s", slot_num, str);
                prevSpeed=stepper.currentSpeed;
            }
        }

        vTaskDelayUntil(&lastWakeTime, c->refreshPeriod);
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