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

#include <generated_files/gen_stepper.h>

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
	uint32_t				homingTimeout;
	uint8_t					pulseWidth;
	int32_t				    maxVal;
    int32_t                 minVal;
	int 					speedReportFlag;
	int 					posReportFlag;
    int 					circularCounterFlag;
    int                     goHomeOnStart;
    int                     active_state;

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

/* Положительный остаток (a mod m), m>0 - для режима кругового счетчика */
static int64_t stepper_wrapmod(int64_t a, int64_t m){
    int64_t r = a % m;
    if(r < 0) r += m;
    return r;
}

/*
    Модуль управления шаговым двигателем сигналами step/dir
    Использует PCNT периферию (ESP32-S3 имеет всего 4 PCNT unit'а суммарно
    на encoderInc + tachometer + stepper).
    slots: 0-5
*/
void configure_stepper(PSTEPPERCONFIG c, int slot_num){
    stdcommand_init(&c->cmds, slot_num);

    /* Включить (1) или выключить (0) модуль. По умолчанию 1. */
    stdcommand_register(&c->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* Состояние модуля - активен (1) или спит (0). Retained. */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");
    /* Если флаг поднят - модуль стартует в выключенном состоянии,
       до прихода action/enable 1 (Конституция §6).
    */
    c->active_state = !get_option_flag_val(slot_num, "disableOnStart");
    ESP_LOGD(TAG, "[stepper_%d] Initial active_state:%d", slot_num, c->active_state);

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

    /* Флаг включает режим кругового счетчика
	*/
	c->circularCounterFlag = get_option_flag_val(slot_num, "circularCounter");
	if(c->circularCounterFlag){
        ESP_LOGD(TAG, "[stepper_%d] circularCounter enable", slot_num);
    }

    /* Флаг базирует двигатель сразу после включения
        иначе, слот ждет команды goHome из вне
	*/
	c->goHomeOnStart = get_option_flag_val(slot_num, "goHomeOnStart");
	if(c->goHomeOnStart){
        ESP_LOGD(TAG, "[stepper_%d] goHomeOnStart enable", slot_num);
    }

    /* Флаг включает рапорты по скорости
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
	c->accel =  get_option_int_val(slot_num, "accel", "step/sek^2", 100, 1, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] accel:%ld", slot_num, c->accel);

    /* Максимальная скорость 
    - шагов в секунду
	*/
	c->maxSpeed =  get_option_int_val(slot_num, "maxSpeed", "step/sek", 100, 1, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] maxSpeed:%ld", slot_num, c->maxSpeed); 
    
    /* Частота обновления
    - раз в секунду
	*/
	c->refreshPeriod =  1000/get_option_int_val(slot_num, "refreshRate", "fps", 20, 1, 100);
    ESP_LOGD(TAG, "[stepper_%d] refreshPeriod:%d", slot_num, c->refreshPeriod);

    /* Скорость базирование
    - шагов в секунду
	- по умолчанию maxSpeed/4
	*/
	c->homingSpeed =  get_option_int_val(slot_num, "homingSpeed", "step/sek", c->maxSpeed / 4, 1, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] homingSpeed:%ld", slot_num, c->homingSpeed);

    /* Таймаут базирования в секундах
    - 0 отключает таймаут
	*/
	c->homingTimeout =  get_option_int_val(slot_num, "homingTimeout", "sek", 30, 0, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] homingTimeout:%ld", slot_num, c->homingTimeout);

    /* Длительность импульса step
    - микросекунды
	- если частота слишком высока для равных уровней HIGH-LOW - урезается до половины периода с варнингом в лог
	*/
	c->pulseWidth =  get_option_int_val(slot_num, "pulseWidth", "us", 5, 1, 10);
    ESP_LOGD(TAG, "[stepper_%d] pulseWidth:%d", slot_num, c->pulseWidth);

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
	c->posReport = stdreport_register(RPTT_string, slot_num, "step", "event/pos");

    /* Рапортует текущую скорость
    - шаг/сек
	*/
	c->speedReport = stdreport_register(RPTT_string, slot_num, "step/sek", "event/speed");

    /* Рапортует текущий режим базирования
	*/
	c->homeReport = stdreport_register(RPTT_string, slot_num, "", "event/homingState");

    /* Команда запускает процесс базирования
    */
    stdcommand_register(&c->cmds, stepCMD_goHome, "action/goHome", PARAMT_none);

    /* Команда включает, режим управления по положению и устанавливает целевое значение в абсолютном режиме
    */
    stdcommand_register(&c->cmds, stepCMD_moveToAbs, "action/moveToAbs", PARAMT_int);

    /* Команда включает, режим управления по положению и устанавливает целевое значение в виде приращения
    */
    stdcommand_register(&c->cmds, stepCMD_moveToInc, "action/moveToInc", PARAMT_int);

    /* Команда включает, режим управления по скорости и устанавливает максимальную скорость движения мотора
    */
    stdcommand_register(&c->cmds, stepCMD_runSpeed, "action/runSpeed", PARAMT_int);

    /* Команда устанавливает максимальную скорость движения мотора
    */
    stdcommand_register(&c->cmds, stepCMD_setMaxSpeed, "action/setMaxSpeed", PARAMT_int);

    /* Команда устанавливает максимальное ускорение мотора
    */
    stdcommand_register(&c->cmds, stepCMD_setAccel, "action/setAccel", PARAMT_int);

    /* Команда экстренной остановки
    */
    stdcommand_register(&c->cmds, stepCMD_stop, "action/stop", PARAMT_none);

    /* Команда остановки с учетом пути торможения
    */
    stdcommand_register(&c->cmds, stepCMD_break, "action/break", PARAMT_none);

    c->homingSensorState=-1;
    /* Команда установки значения датчика нулевого положения
    */
    stdcommand_register(&c->cmds, stepCMD_setHomingSensor, "action/setHomingSensor", PARAMT_int);

}


#define HOMING_WAITING 1
#define HOMING_TO_SENSOR 2 
#define HOMING_OUT_SENSOR 3

void stepper_task(void *arg){
    PSTEPPERCONFIG c = calloc(1, sizeof(STEPPERCONFIG));
    STDCOMMAND_PARAMS       params = { 0 };

    int slot_num = (int)(intptr_t)arg;
	
    configure_stepper(c, slot_num);
    c->homingSensorState = -1;

    stepper_t stepper = STEPPER_DEFAULT();
    stepper.dirPin = SLOTS_PIN_MAP[slot_num][0];
    stepper.stepPin = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
    
    stepper.accel = c->accel;
    stepper.maxSpeed = c->maxSpeed;

	esp_err_t step_err = stepper_init(&stepper, stepper.stepPin, stepper.dirPin, c->pulseWidth);
	if (step_err != ESP_OK) {
		ESP_LOGW(TAG, "PCNT unit limit reached (slot:%d), task terminated. err:%d",
		         slot_num, step_err);
		vTaskDelete(NULL);
	}

    esp_rom_gpio_pad_select_gpio(SLOTS_PIN_MAP[slot_num][2]);
    gpio_set_direction(SLOTS_PIN_MAP[slot_num][2], GPIO_MODE_OUTPUT);
    gpio_set_level(SLOTS_PIN_MAP[slot_num][2], 1);// для модулей out_2ch пин nsleep
    
    int32_t prevPos=0;
    int32_t prevSpeed=0;

    int homingProcedureState = HOMING_WAITING;
    TickType_t homingStartTick = 0;

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

        /* Если модуль выключен - принимаем только action/enable, все команды
           движения игнорируем (Конституция §6). */
        if (!c->active_state && cmd != -1 && cmd != STDCMD_ENABLE) {
            ESP_LOGD(TAG, "[stepper_%d] disabled, ignoring cmd:%d", slot_num, cmd);
            cmd = -1;
        }

        switch (cmd){
            case -1: // none
                break;

            case STDCMD_ENABLE:
                if (params.count > 0) {
                    int new_state = params.p[0].i ? 1 : 0;
                    if (new_state != c->active_state) {
                        c->active_state = new_state;
                        ESP_LOGD(TAG, "[stepper_%d] enable:%d", slot_num, c->active_state);
                        /* event/enable публикуется автоматически stdcommand_receive */
                        if (!c->active_state) {
                            /* Экстренная остановка при выключении (Конституция §6) */
                            stepper_stop(&stepper);
                        }
                    }
                }
                break;

            case stepCMD_goHome:
                homingProcedureState=HOMING_WAITING;
                c->state = GOING_HOME;
                ESP_LOGD(TAG, "[stepper_%d] lets go home", slot_num);
                break;

            case stepCMD_moveToInc:
                // if((c->state==NOT_HOMED)||(c->state==GOING_HOME)){
                //     break;
                // }
                c->state = RUN_POS;
                stepper.runSpeedFlag = 0;
                stepper.maxSpeed = c->maxSpeed;             // восстанавливаем cap позиционирования (runSpeed мог его испортить)
                target = stepper.currentPos + atoi(cmd_arg); // atoi сам разбирает знак
                if(!c->circularCounterFlag){                 // в круговом режиме ход не ограничиваем
                    if(target > c->maxVal) target = c->maxVal;
                    if(target < c->minVal) target = c->minVal;
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
                stepper.maxSpeed = c->maxSpeed;             // восстанавливаем cap позиционирования
                target = atoi(cmd_arg);
                if(c->circularCounterFlag && (c->maxVal > c->minVal)){
                    // круговой режим: кратчайший путь к цели с учетом заворота диапазона
                    int64_t range = (int64_t)c->maxVal - (int64_t)c->minVal;
                    int64_t cur   = stepper.currentPos;
                    int64_t fwd   = stepper_wrapmod((int64_t)target - cur, range); // путь вперед [0,range)
                    int64_t delta = (fwd <= range - fwd) ? fwd : (fwd - range);    // короче вперед или назад
                    target = (int32_t)(cur + delta);
                }else{
                    if(target > c->maxVal) target = c->maxVal;   // ограничение хода
                    if(target < c->minVal) target = c->minVal;
                }
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
                // Меняем постоянный cap скорости позиционирования (magnitude).
                c->maxSpeed = abs(atoi(cmd_arg));
                stepper.maxSpeed = c->maxSpeed;
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

        if(c->active_state && c->state==GOING_HOME){
            if(homingProcedureState==HOMING_WAITING){
                stdreport_s(c->homeReport, "homing");
                homingStartTick = xTaskGetTickCount();
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
            }else if(c->homingTimeout>0 &&
                     (xTaskGetTickCount()-homingStartTick) > pdMS_TO_TICKS(c->homingTimeout*1000)){
                // датчик не найден за отведенное время - аварийно прекращаем базирование
                stepper_stop(&stepper);
                stepper.maxSpeed = c->maxSpeed;
                stepper.accel = c->accel;
                c->state = NOT_HOMED;
                homingProcedureState = HOMING_WAITING;
                stdreport_s(c->homeReport, "timeout");
                ESP_LOGW(TAG, "[stepper_%d] homing timeout", slot_num);
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

        if (c->active_state) {
            stepper_speedUpdate(&stepper, c->refreshPeriod);
        }
        stepper_getCurrentPos(&stepper);
        //ESP_LOGD(TAG, "currentPos: %ld prevPos:%ld dir:%d", stepper.currentPos,  stepper.pcnt_prevPos,  stepper.dir);

        if(c->posReportFlag){
            // absPos - истинная позиция (переживает хак runSpeed); в круговом режиме заворачиваем
            int32_t reportPos = stepper.absPos;
            if(c->circularCounterFlag && (c->maxVal > c->minVal)){
                int64_t range = (int64_t)c->maxVal - (int64_t)c->minVal;
                reportPos = (int32_t)((int64_t)c->minVal + stepper_wrapmod((int64_t)stepper.absPos - c->minVal, range));
            }
            if(reportPos!=prevPos){
                char str[15];
                sprintf(str, "%ld", reportPos);
				stdreport_s(c->posReport, str);
                //ESP_LOGD(TAG, "Stepper_%d curentPos:%s", slot_num, str);
                prevPos=reportPos;
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
	char tmpString[60];
	sprintf(tmpString, "task_stepper_%d", slot_num);
	//xTaskCreate(stepper_task, tmpString, 1024*12, (void*)(intptr_t)slot_num,configMAX_PRIORITIES, NULL);
    xTaskCreatePinnedToCore(stepper_task, tmpString, 1024*12, (void*)(intptr_t)slot_num,configMAX_PRIORITIES-6, NULL,1);

	ESP_LOGD(TAG,"stepper task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_stepper()
{
	return manifesto;
}